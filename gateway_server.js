const express = require('express');
const axios = require('axios');
const sharp = require('sharp');
const fs = require('fs');
const path = require('path');
const TelegramBot = require('node-telegram-bot-api');

const app = express();
const port = 3000;

// --- Middleware ---
// JSON parser for registration endpoint
app.use(express.json());
// Raw body parser for receiving the image from the ESP32-CAM
app.use('/upload-image', express.raw({
  type: 'image/jpeg',
  limit: '10mb' 
}));


// --- State ---
const devices = new Map();
const MAX_DEVICES = 10;
const pendingCaptures = new Map();

const CHAT_ID_FILE = path.join(__dirname, 'chat_id.txt');
let targetChatId = null;

if (fs.existsSync(CHAT_ID_FILE)) {
  const content = fs.readFileSync(CHAT_ID_FILE, 'utf8').trim();
  if (content) targetChatId = content;
  console.log(`Loaded target Chat ID: ${targetChatId}`);
}

// --- Telegram Bot ---
const token = '7910361449:AAFMjzZxkDQAg1y6oeIJ0gVapBXbd2e11DU';
// Create a bot that uses 'polling' to fetch new updates
const bot = new TelegramBot(token, { polling: true });

bot.onText(/\/start/, (msg) => {
  const chatId = msg.chat.id;
  const text = "Welcome to the ESP32-CAM Gateway Bot!\n\n" +
               "Available commands:\n" +
               "/devices - List registered devices\n" +
               "/photo - Capture photo from a device\n" +
               "/setThisUserId - Set this chat for notifications\n";
  bot.sendMessage(chatId, text);
  //
  // bot.sendMessage(chatId, 'hello world');
});

bot.onText(/\/devices/, (msg) => {
  const chatId = msg.chat.id;
  if (devices.size === 0) {
    return bot.sendMessage(chatId, 'No devices registered.');
  }
  let message = 'Registered Devices:\n';
  devices.forEach((device) => {
    message += `IP: ${device.ip}, MAC: ${device.mac}\n`;
  });
  bot.sendMessage(chatId, message);
});

bot.onText(/\/photo/, (msg) => {
  const chatId = msg.chat.id;
  if (devices.size === 0) {
    return bot.sendMessage(chatId, 'No devices registered.');
  }
  let message = 'Registered Devices:\n';
  const inline_keyboard = [];
  devices.forEach((device) => {
    message += `IP: ${device.ip}, MAC: ${device.mac}\n`;
    inline_keyboard.push([{ text: `Capture ${device.mac}`, callback_data: `capture_${device.mac}` }]);
  });
  const opts = {
    reply_markup: {
      inline_keyboard: inline_keyboard
    }
  };
  bot.sendMessage(chatId, message, opts);
});

bot.onText(/\/setThisUserId/, (msg) => {
  const chatId = msg.chat.id;
  targetChatId = chatId;
  fs.writeFileSync(CHAT_ID_FILE, String(chatId));
  bot.sendMessage(chatId, `Notification target set to this chat ID: ${chatId}`);
});

// Handle button clicks from inline keyboards
bot.on('callback_query', async (callbackQuery) => {
  const msg = callbackQuery.message;
  const data = callbackQuery.data;
  const chatId = msg.chat.id;

  if (data.startsWith('capture_')) {
    const mac = data.split('_')[1];
    const targetDevice = devices.get(mac);

    if (!targetDevice) {
      return bot.answerCallbackQuery(callbackQuery.id, { text: 'Device not found!', show_alert: true });
    }

    const cameraUrl = `http://${targetDevice.ip}/trigger-photo`;
    console.log(`Telegram triggering capture on ${cameraUrl}`);

    let sentMsg;
    try {
      // 1. Send "Waiting" message and register pending capture BEFORE triggering camera
      // This prevents the race condition where upload arrives before map is set
      sentMsg = await bot.sendMessage(chatId, `Triggered capture on ${targetDevice.ip} (${mac}). Waiting for upload...`);
      pendingCaptures.set(targetDevice.ip, { chatId, messageId: sentMsg.message_id });

      // 2. Trigger the camera
      const response = await axios.get(cameraUrl);
      if (response.status === 202) {
        bot.answerCallbackQuery(callbackQuery.id, { text: 'Command sent!' });
      }
    } catch (error) {
      console.error('Telegram trigger failed:', error.message);
      bot.answerCallbackQuery(callbackQuery.id, { text: 'Failed to connect', show_alert: true });
      if (sentMsg) {
        bot.deleteMessage(chatId, sentMsg.message_id).catch(() => {});
        pendingCaptures.delete(targetDevice.ip);
      }
    }
  }
});

// --- Directory for Captures ---
const capturesDir = path.join(__dirname, 'captures');
if (!fs.existsSync(capturesDir)) {
  fs.mkdirSync(capturesDir);
  console.log(`Created directory: ${capturesDir}`);
}

// --- Endpoints ---

// Endpoint for ESP32-CAM to register its IP
app.post('/register', (req, res) => {
  const { ip, mac } = req.body;
  if (!ip || !mac) {
    console.error('Registration failed: Missing IP or MAC in request.');
    return res.status(400).send('Missing IP or MAC address.');
  }

  if (devices.has(mac)) {
    devices.set(mac, { ip, mac, lastSeen: new Date() });
    console.log(`Updated ESP32-CAM: IP=${ip}, MAC=${mac}`);
    return res.status(200).send('Device updated successfully.');
  }

  if (devices.size >= MAX_DEVICES) {
    return res.status(403).send('Max registered devices limit reached.');
  }

  devices.set(mac, { ip, mac, lastSeen: new Date() });
  console.log(`Registered ESP32-CAM: IP=${ip}, MAC=${mac}. Total: ${devices.size}`);
  res.status(200).send('Device registered successfully.');
});

// New endpoint for ESP32-CAM to POST the image data to
app.post('/upload-image', async (req, res) => {
  console.log(`Received image push from ESP32-CAM, size: ${req.body.length} bytes`);

  // Handle Telegram notification
  let remoteIp = req.ip || req.socket.remoteAddress;
  if (remoteIp && remoteIp.includes('::ffff:')) {
    remoteIp = remoteIp.split('::ffff:')[1];
  }
  console.log(`Processing upload from IP: ${remoteIp}. Pending match: ${pendingCaptures.has(remoteIp)}`);

  const pending = pendingCaptures.get(remoteIp);
  if (pending) {
    try {
      await bot.deleteMessage(pending.chatId, pending.messageId);
      await bot.sendPhoto(pending.chatId, req.body);
      pendingCaptures.delete(remoteIp);
    } catch (err) {
      console.error('Error updating Telegram:', err.message);
    }
  }

  // Check for Motion Detection Header
  if (req.get('X-Motion-Detected') === 'true') {
    console.log('Motion detected from ESP32!');
    if (targetChatId) {
      bot.sendPhoto(targetChatId, req.body, { caption: 'Motion Detected!' })
        .catch(err => console.error('Telegram send error:', err.message));
    }
  }

  try {
    const timestamp = new Date().toISOString().replace(/[:.]/g, '-');
    const imagePath = path.join(capturesDir, `capture-${timestamp}.png`);

    // Convert the received JPEG buffer to a PNG file
    await sharp(req.body)
      .png()
      .toFile(imagePath);

    console.log(`Successfully converted and saved image to ${imagePath}`);
    res.status(200).send('Image uploaded and saved successfully.');

  } catch (error) {
    console.error('Failed to process or save uploaded image:', error.message);
    res.status(500).send('Failed to process image.');
  }
});


// This endpoint now just triggers the camera
app.get('/capture', async (req, res) => {
  const mac = req.query.mac;
  let targetDevice;

  if (mac) {
    targetDevice = devices.get(mac);
    if (!targetDevice) {
      return res.status(404).send('Device not found.');
    }
  } else {
    if (devices.size === 0) {
      console.error('Capture trigger failed: No ESP32-CAM registered.');
      return res.status(503).send('Capture trigger failed: ESP32-CAM not registered.');
    } else if (devices.size === 1) {
      targetDevice = devices.values().next().value;
    } else {
      return res.status(400).send('Multiple devices registered. Please specify ?mac=...');
    }
  }
  
  const cameraUrl = `http://${targetDevice.ip}/trigger-photo`;
  console.log(`Sending capture trigger to ${cameraUrl}`);

  try {
    const response = await axios.get(cameraUrl);
    // Expecting a 202 Accepted from the camera
    if (response.status === 202) {
      console.log('Successfully triggered camera. Waiting for image push...');
      res.status(200).send('Capture command sent to camera. Image will be uploaded shortly.');
    } else {
      throw new Error(`Camera responded with status ${response.status}`);
    }
  } catch (error) {
    console.error('Failed to trigger ESP32-CAM:', error.message);
    res.status(500).send(`Failed to trigger ESP32-CAM at ${targetDevice.ip}. Is it online?`);
  }
});

// Simple status endpoint to check if camera is registered
app.get('/status', (req, res) => {
    if (devices.size > 0) {
        res.status(200).json({
            status: 'online',
            count: devices.size,
            devices: Array.from(devices.values())
        });
    } else {
        res.status(404).json({
            status: 'offline',
            message: 'No ESP32-CAM has registered yet.'
        });
    }
});

app.get('/', (req, res) => {
  res.status(200).send('ESP32-CAM Gateway Server is running.');
});

app.listen(port, () => {
  console.log(`Gateway server listening on port ${port}`);
  console.log('Waiting for ESP32-CAM to register...');
});
