#include <Arduino.h>
#include "WiFi.h"
#include <WebServer.h>
#include <HTTPClient.h>
#include "esp_camera.h"

// This is the code for the ESP32-CAM node, modified to use FreeRTOS
// to prevent blocking operations from making the web server unresponsive.

// Pin definition for AI-THINKER board
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

const char* ssid = "BatuKhan";
const char* password = "momoygemoy";

// --- Gateway Details ---
const char* gatewayAddress = "http://esp32gate/register"; 

WebServer server(80);

// State for advertising to gateway
bool isRegistered = false;

// Task handles
TaskHandle_t httpServerTaskHandle = NULL;
TaskHandle_t advertiseTaskHandle = NULL;

void advertiseDevice() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  HTTPClient http;
  http.begin(gatewayAddress);
  http.addHeader("Content-Type", "application/json");

  String ip = WiFi.localIP().toString();
  String mac = WiFi.macAddress();
  String json = "{\"ip\":\"" + ip + "\",\"mac\":\"" + mac + "\"}";

  if (isRegistered) {
    Serial.println("Re-advertising device to gateway...");
  } else {
    Serial.println("Advertising device to gateway...");
  }

  int httpResponseCode = http.POST(json);

  if (httpResponseCode == 200) {
    if (!isRegistered) {
      Serial.println("Device successfully registered with gateway!");
      isRegistered = true;
    } else {
      Serial.println("Device re-advertised successfully.");
    }
  } else {
    Serial.print("Gateway advertising failed, error: ");
    Serial.println(httpResponseCode);
  }
  http.end();
}

void advertiseTask(void *pvParameters) {
  Serial.println("Advertise task started");
  const TickType_t advertIntervalUnregistered = pdMS_TO_TICKS(5000);
  const TickType_t advertIntervalRegistered = pdMS_TO_TICKS(15000);

  for (;;) {
    TickType_t delayTime = isRegistered ? advertIntervalRegistered : advertIntervalUnregistered;
    advertiseDevice();
    vTaskDelay(delayTime);
  }
}

void httpServerTask(void *pvParameters) {
    Serial.println("HTTP server task started");
    for (;;) {
        server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(10)); // Small delay to yield to other tasks
    }
}


void handleGetInfo() {
  String ip = WiFi.localIP().toString();
  String mac = WiFi.macAddress();
  String json = "{\"ip\":\"" + ip + "\",\"mac\":\"" + mac + "\"}";
  server.send(200, "application/json", json);
}

void handleHello() {
  server.send(200, "text/plain", "hello world");
}

void handleTakePhoto() {
  camera_fb_t * fb = NULL;

  fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Failed to capture photo");
    server.send(500, "text/plain", "Failed to capture photo");
    return;
  }

  server.setContentLength(fb->len);
  server.sendHeader("Content-Type", "image/jpeg");
  server.sendHeader("Content-Disposition", "attachment; filename=picture.jpeg");
  server.sendContent((const char *)fb->buf, fb->len);

  esp_camera_fb_return(fb);
  fb = NULL;
}

void setup() {
  Serial.begin(115200);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  
  config.frame_size = FRAMESIZE_UXGA;
  config.jpeg_quality = 12; // Lower quality for faster capture
  config.fb_count = 1;

  // Camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    ESP.restart();
  }

  sensor_t * s = esp_camera_sensor_get();
  // Using a smaller frame size is more reliable.
  s->set_framesize(s, FRAMESIZE_SVGA); // 800x600

  server.on("/getinfo", handleGetInfo);
  server.on("/hello", handleHello);
  server.on("/takephoto", handleTakePhoto);
  server.begin();
  Serial.println("HTTP server started");

  // Create RTOS tasks and pin them to core 1
  xTaskCreatePinnedToCore(
      httpServerTask,
      "HTTPServer",
      10000,
      NULL,
      1,
      &httpServerTaskHandle,
      1);

  xTaskCreatePinnedToCore(
      advertiseTask,
      "Advertise",
      5000,
      NULL,
      1,
      &advertiseTaskHandle,
      1);
}

void loop() {
  // The default Arduino loop task is no longer needed.
  vTaskDelete(NULL);
}