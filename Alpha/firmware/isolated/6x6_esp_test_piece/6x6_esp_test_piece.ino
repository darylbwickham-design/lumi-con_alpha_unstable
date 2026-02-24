#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>

// -------- Pico UART packet settings (must match Pico) --------
constexpr uint32_t PICO_BAUD = 115200;
constexpr uint8_t KEY_COUNT = 36;

// -------- Short/Long press threshold --------
constexpr uint32_t LONG_PRESS_MS = 600;

// -------- Lumia plugin endpoint (your PC) --------
const char* PLUGIN_HOST = "192.168.1.87";
constexpr uint16_t PLUGIN_PORT = 8787;

// Optional: if you set "Shared Secret" in the plugin settings, put it here.
const char* PLUGIN_SECRET = ""; // e.g. "mysecret123" or "" for none

// -------- State tracking --------
uint32_t pressStart[KEY_COUNT] = {};
bool isDown[KEY_COUNT] = {};

// -------- UART packet decoder: A5, type(1/0), key(0..35), xor --------
bool readPicoPacket(uint8_t &type, uint8_t &key) {
  static uint8_t state = 0;
  static uint8_t b1 = 0, b2 = 0;

  while (Serial.available()) {
    uint8_t b = (uint8_t)Serial.read();
    switch (state) {
      case 0: if (b == 0xA5) state = 1; break;
      case 1: b1 = b; state = 2; break;           // type
      case 2: b2 = b; state = 3; break;           // key
      case 3: {                                   // xor
        uint8_t chk = (uint8_t)(0xA5 ^ b1 ^ b2);
        state = 0;
        if (b == chk) { type = b1; key = b2; return true; }
        break;
      }
    }
  }
  return false;
}

// -------- HTTP POST with timeout + gentle retries --------
bool postEventToPlugin(uint8_t eventNumber) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;

  String url = String("http://") + PLUGIN_HOST + ":" + String(PLUGIN_PORT) + "/event";
  String body = String("{\"event\":") + String(eventNumber) + "}";

  // 3 attempts with exponential backoff (200ms, 400ms, 800ms)
  uint32_t backoff = 200;
  for (uint8_t attempt = 1; attempt <= 3; attempt++) {
    http.setTimeout(1200); // ms (no AbortSignal used)
    if (!http.begin(client, url)) {
      http.end();
      delay(backoff);
      backoff *= 2;
      continue;
    }

    http.addHeader("Content-Type", "application/json");
    if (PLUGIN_SECRET && PLUGIN_SECRET[0] != '\0') {
      http.addHeader("X-Matrix-Secret", PLUGIN_SECRET);
    }

    int code = http.POST((uint8_t*)body.c_str(), body.length());
    http.end();

    if (code >= 200 && code < 300) return true;

    delay(backoff);
    backoff *= 2;
    yield();
  }

  return false;
}

void setup() {
  // IMPORTANT: Serial is also RX pin (GPIO3). This is OK since we’re not using USB logs.
  // If you *need* USB logging, you’ll need SoftwareSerial or a different strategy.
  Serial.begin(PICO_BAUD);
  delay(200);

  // WiFi setup (won't show AP if already configured and connects)
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  bool ok = wm.autoConnect("MatrixRelay-Setup");
  (void)ok; // if it times out, we still keep running

  // Optional: quick LED blink to show booted
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
}

void loop() {
  uint8_t type, key;
  if (readPicoPacket(type, key)) {
    if (key >= KEY_COUNT) return;

    uint32_t now = millis();

    if (type == 1) {
      // PRESS
      if (!isDown[key]) {
        isDown[key] = true;
        pressStart[key] = now;
      }
    } else {
      // RELEASE
      if (isDown[key]) {
        isDown[key] = false;

        uint32_t held = now - pressStart[key];
        uint8_t eventOut = (held >= LONG_PRESS_MS) ? (uint8_t)(key + 36) : key;

        // Send to Lumia plugin
        bool okSend = postEventToPlugin(eventOut);

        // Minimal visible feedback via onboard LED:
        // blink once on success, twice on failure
        if (okSend) {
          digitalWrite(LED_BUILTIN, LOW); delay(30);
          digitalWrite(LED_BUILTIN, HIGH);
        } else {
          for (int i = 0; i < 2; i++) {
            digitalWrite(LED_BUILTIN, LOW); delay(30);
            digitalWrite(LED_BUILTIN, HIGH); delay(60);
          }
        }
      }
    }
  }

  yield();
}