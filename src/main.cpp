#include <Arduino.h>
#include "WiFi.h"
#include <WebServer.h>
#include <HTTPClient.h>
#include "esp_camera.h"

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
const char* gatewayUploadUrl = "http://esp32gate/upload-image";

WebServer server(80);

// --- State Management ---
bool isRegistered = false;
unsigned long lastAdvertTime = 0;
const long advertIntervalUnregistered = 5000; // 5 seconds
const long advertIntervalRegistered = 30000;   // 30 seconds
volatile bool takeAndSendPhoto = false; // Flag to trigger async photo capture and send

void advertiseDevice() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  unsigned long currentMillis = millis();
  long currentInterval = isRegistered ? advertIntervalRegistered : advertIntervalUnregistered;

  if (currentMillis - lastAdvertTime >= currentInterval) {
    lastAdvertTime = currentMillis;

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
}

// This function now runs from the main loop when triggered
void captureAndPushImage() {
  if (!takeAndSendPhoto) {
    return;
  }
  
  takeAndSendPhoto = false; // Reset flag
  Serial.println("Starting asynchronous image capture and upload...");

  camera_fb_t * fb = NULL;
  for(uint8_t i = 0; i < 3; i++) {
    fb = esp_camera_fb_get();
    esp_camera_fb_return(fb);
  }

  fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Failed to capture photo");
    return;
  }

  HTTPClient http;
  http.begin(gatewayUploadUrl);
  http.addHeader("Content-Type", "image/jpeg");

  Serial.printf("Pushing image to gateway, size: %zu bytes\n", fb->len);
  int httpResponseCode = http.POST(fb->buf, fb->len);

  if (httpResponseCode == 200) {
    Serial.println("Image uploaded successfully.");
  } else {
    Serial.print("Image upload failed, error: ");
    Serial.println(httpResponseCode);
  }

  http.end();
  esp_camera_fb_return(fb);
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

// This handler just sets a flag and returns immediately
void handleTriggerPhoto() {
  server.send(202, "text/plain", "Accepted: Photo capture triggered.");
  takeAndSendPhoto = true;
  Serial.println("Received trigger request. Will capture and send photo shortly.");
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
  config.jpeg_quality = 10;
  config.fb_count = 2;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  server.on("/getinfo", handleGetInfo);
  server.on("/hello", handleHello);
  server.on("/trigger-photo", handleTriggerPhoto); // Changed from /takephoto
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();
  advertiseDevice();
  captureAndPushImage(); // Check if we need to send a photo
}