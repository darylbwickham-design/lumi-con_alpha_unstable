/*
  Lumi-Con ESP8266 firmware v0.0.2  (between your v0.0.1 and v0.0.3)
  ------------------------------------------------------------------
  Adds:
  - /health endpoint on ESP
  - deviceId + seq + extra telemetry in POST /event payload (backwards compatible)
  - Self-test:
      - TFT shows IP + last key pressed
      - LED blink codes:
          WiFi OK: 2 quick
          PC reachable (/health on plugin): 3 quick
          PC unreachable: 3 slow
          Pico detected (first UART packet): 1 quick
  - Factory reset: hold Key 0 during boot to wipe WiFiManager credentials

  Keeps:
  - TFT endpoints: /msg /status /clear /ui
  - Pico UART protocol: A5 type key xor
  - Event mapping: short=0..35, long=36..71
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

// ===================== CONFIG =====================

// ---- Lumia plugin endpoint (your PC) ----
const char* PLUGIN_HOST = "192.168.1.87";
constexpr uint16_t PLUGIN_PORT = 8787;
const char* PLUGIN_SECRET = ""; // optional header X-Matrix-Secret

// ---- Pico UART ----
constexpr uint32_t PICO_BAUD = 115200;
constexpr uint8_t KEY_COUNT = 36;
constexpr uint32_t LONG_PRESS_MS = 600;

// ---- Factory reset (hold this key on boot) ----
constexpr uint8_t FACTORY_RESET_KEY = 0;
constexpr uint32_t FACTORY_RESET_HOLD_MS = 1200;
constexpr uint32_t FACTORY_RESET_WINDOW_MS = 2500;

// ---- TFT pins ----
#define TFT_CS   D2
#define TFT_DC   D1
#define TFT_RST  D0
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
#define ST7735_TAB INITR_GREENTAB

// ---- Screen layout ----
static const uint16_t BG = ST77XX_BLACK;
static const uint16_t FG = ST77XX_WHITE;

static const int SCREEN_W = 160;
static const int SCREEN_H = 128;

static const int CHAR_W = 6;
static const int LINE_H = 8;
static const int MAX_COLS = SCREEN_W / CHAR_W;
static const int MAX_LINES = SCREEN_H / LINE_H;

static const int STATUS_LINES = 1;
static const int CHAT_LINES = MAX_LINES - STATUS_LINES;

// ===================== GLOBALS =====================

ESP8266WebServer server(80);

// Device identity
String deviceId;

// Chat buffer
String chatBuf[CHAT_LINES];
int chatCount = 0;

// Status line
String statusBase = "";      // e.g. "IP:192.168.1.50"
String lastKeyText = "K:--"; // e.g. "K:12L"

// Key press tracking
uint32_t pressStart[KEY_COUNT] = {};
bool isDown[KEY_COUNT] = {};

// Pico detected flag
bool picoSeen = false;

// Sequence counter + last result
uint32_t seqCounter = 0;
uint32_t lastSeqSent = 0;
uint32_t lastAckSeq = 0;
bool lastPostOk = false;

// ===================== LED HELPERS (active-low LED) =====================

void ledOn()  { digitalWrite(LED_BUILTIN, LOW); }
void ledOff() { digitalWrite(LED_BUILTIN, HIGH); }

void blink(uint8_t times, uint16_t onMs, uint16_t offMs) {
  for (uint8_t i = 0; i < times; i++) {
    ledOn();  delay(onMs);
    ledOff(); delay(offMs);
    yield();
  }
}

// ===================== TFT HELPERS =====================

String truncateCols(const String& s) {
  if ((int)s.length() <= MAX_COLS) return s;
  return s.substring(0, MAX_COLS);
}

void addChatLine(const String& line) {
  if (chatCount < CHAT_LINES) {
    chatBuf[chatCount++] = line;
  } else {
    for (int i = 1; i < CHAT_LINES; i++) chatBuf[i - 1] = chatBuf[i];
    chatBuf[CHAT_LINES - 1] = line;
  }
}

void redrawScreen() {
  tft.fillScreen(BG);
  tft.setTextWrap(false);
  tft.setTextSize(1);
  tft.setTextColor(FG, BG);

  // Status (single line): "<statusBase> <lastKeyText>"
  String s = statusBase;
  if (s.length()) s += " ";
  s += lastKeyText;
  s = truncateCols(s);

  tft.setCursor(0, 0);
  tft.print(s);

  // Chat area (bottom aligned)
  int startIndex = (chatCount > CHAT_LINES) ? (chatCount - CHAT_LINES) : 0;
  int linesToDraw = chatCount - startIndex;

  int chatRegionH = CHAT_LINES * LINE_H;
  int yStart = (STATUS_LINES * LINE_H) + (chatRegionH - linesToDraw * LINE_H);
  if (yStart < STATUS_LINES * LINE_H) yStart = STATUS_LINES * LINE_H;

  int y = yStart;
  for (int i = startIndex; i < chatCount; i++) {
    tft.setCursor(0, y);
    tft.print(truncateCols(chatBuf[i]));
    y += LINE_H;
    if (y >= SCREEN_H) break;
  }
}

String urlDecode(const String& in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '+') out += ' ';
    else if (c == '%' && i + 2 < in.length()) {
      auto hexVal = [](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return 10 + (h - 'a');
        if (h >= 'A' && h <= 'F') return 10 + (h - 'A');
        return 0;
      };
      char decoded = (hexVal(in[i + 1]) << 4) | hexVal(in[i + 2]);
      out += decoded;
      i += 2;
    } else out += c;
  }
  return out;
}

void clearAll() {
  chatCount = 0;
  statusBase = "";
  lastKeyText = "K:--";
  tft.fillScreen(BG);
}

void pushWrappedChat(const String& msgRaw) {
  String msg = msgRaw;
  msg.replace("\r", "");

  int start = 0;
  while (start <= (int)msg.length()) {
    int nl = msg.indexOf('\n', start);
    String segment = (nl == -1) ? msg.substring(start) : msg.substring(start, nl);

    while (segment.endsWith(" ")) segment.remove(segment.length() - 1);

    int idx = 0;
    while (idx < (int)segment.length()) {
      int remaining = segment.length() - idx;

      if (remaining <= MAX_COLS) {
        addChatLine(segment.substring(idx));
        idx = segment.length();
        break;
      }

      int cut = idx + MAX_COLS;
      int space = segment.lastIndexOf(' ', cut);
      if (space <= idx) {
        addChatLine(segment.substring(idx, idx + MAX_COLS));
        idx += MAX_COLS;
      } else {
        addChatLine(segment.substring(idx, space));
        idx = space + 1;
      }
    }

    if (nl == -1) break;
    start = nl + 1;
  }

  redrawScreen();
}

// ===================== HTTP HANDLERS =====================

void handleRoot() {
  server.send(200, "text/plain",
    "Lumi-Con ESP OK\n"
    "GET /msg?t=Hello\n"
    "GET /status?t=OK\n"
    "GET /clear\n"
    "POST /ui {\"channel\":\"chat|status|clear\",\"text\":\"...\"}\n"
    "GET /health\n"
  );
}

void handleMsg() {
  if (!server.hasArg("t")) return server.send(400, "text/plain", "Missing 't'");
  String text = urlDecode(server.arg("t"));
  if (!text.length()) return server.send(400, "text/plain", "Empty");
  pushWrappedChat(text);
  server.send(200, "text/plain", "OK");
}

void handleStatus() {
  if (!server.hasArg("t")) return server.send(400, "text/plain", "Missing 't'");
  statusBase = urlDecode(server.arg("t"));
  redrawScreen();
  server.send(200, "text/plain", "OK");
}

void handleClear() {
  clearAll();
  server.send(200, "text/plain", "CLEARED");
}

String jsonFindString(const String& body, const String& key) {
  int k = body.indexOf("\"" + key + "\"");
  if (k < 0) return "";
  int colon = body.indexOf(':', k);
  if (colon < 0) return "";
  int q1 = body.indexOf('"', colon + 1);
  if (q1 < 0) return "";
  int q2 = body.indexOf('"', q1 + 1);
  if (q2 < 0) return "";
  return body.substring(q1 + 1, q2);
}

void handleUi() {
  if (server.method() != HTTP_POST) return server.send(405, "text/plain", "POST only");
  String body = server.arg("plain");
  body.trim();
  if (!body.length()) return server.send(400, "text/plain", "Empty body");

  String channel = jsonFindString(body, "channel");
  String text = jsonFindString(body, "text");

  if (channel == "clear") {
    clearAll();
    return server.send(200, "text/plain", "OK");
  }

  if (channel == "status") {
    if (!text.length()) return server.send(400, "text/plain", "Missing text");
    statusBase = text;
    redrawScreen();
    return server.send(200, "text/plain", "OK");
  }

  if (!text.length()) return server.send(400, "text/plain", "Missing text");
  pushWrappedChat(text);
  server.send(200, "text/plain", "OK");
}

void handleHealth() {
  String ip = WiFi.localIP().toString();
  long rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;

  String json = "{";
  json += "\"ok\":true";
  json += ",\"deviceId\":\"" + deviceId + "\"";
  json += ",\"ip\":\"" + ip + "\"";
  json += ",\"rssi\":" + String(rssi);
  json += ",\"uptimeMs\":" + String(millis());
  json += ",\"lastKey\":\"" + lastKeyText + "\"";
  json += ",\"lastSeq\":" + String(lastSeqSent);
  json += ",\"lastAck\":" + String(lastAckSeq);
  json += ",\"lastPostOk\":" + String(lastPostOk ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

// ===================== PICO UART PACKET DECODE (UNCHANGED) =====================

bool readPicoPacket(uint8_t &type, uint8_t &key) {
  static uint8_t state = 0;
  static uint8_t b1 = 0, b2 = 0;

  while (Serial.available()) {
    uint8_t b = (uint8_t)Serial.read();
    switch (state) {
      case 0: if (b == 0xA5) state = 1; break;
      case 1: b1 = b; state = 2; break;
      case 2: b2 = b; state = 3; break;
      case 3: {
        uint8_t chk = (uint8_t)(0xA5 ^ b1 ^ b2);
        state = 0;
        if (b == chk) { type = b1; key = b2; return true; }
        break;
      }
    }
  }
  return false;
}

// ===================== FACTORY RESET VIA KEY HOLD =====================

bool checkFactoryResetHold() {
  uint32_t start = millis();
  bool keyDown = false;
  uint32_t downAt = 0;

  while (millis() - start < FACTORY_RESET_WINDOW_MS) {
    uint8_t type, key;
    if (readPicoPacket(type, key)) {
      if (!picoSeen) { picoSeen = true; blink(1, 40, 80); } // pico detected
      if (key == FACTORY_RESET_KEY) {
        if (type == 1) { keyDown = true; downAt = millis(); }
        else { keyDown = false; }
      }
    }

    if (keyDown && (millis() - downAt) >= FACTORY_RESET_HOLD_MS) return true;
    yield();
  }

  return false;
}

// ===================== PLUGIN REACHABILITY CHECK =====================

bool checkPcReachable() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client;
  HTTPClient http;

  String url = String("http://") + PLUGIN_HOST + ":" + String(PLUGIN_PORT) + "/health";
  http.setTimeout(1200);
  if (!http.begin(client, url)) { http.end(); return false; }
  int code = http.GET();
  http.end();
  return (code >= 200 && code < 300);
}

// ===================== POST EVENT (backwards compatible) =====================

bool parseAckSeq(const String& body, uint32_t &outSeq) {
  int i = body.indexOf("\"seq\"");
  if (i < 0) return false;
  int colon = body.indexOf(':', i);
  if (colon < 0) return false;

  int j = colon + 1;
  while (j < (int)body.length() && (body[j] == ' ' || body[j] == '\t')) j++;

  String num;
  while (j < (int)body.length() && isDigit(body[j])) {
    num += body[j];
    j++;
  }
  if (!num.length()) return false;
  outSeq = (uint32_t)num.toInt();
  return true;
}

bool postEventToPlugin(uint8_t eventNumber, uint8_t keyIndex, const String& pressKind, uint32_t heldMs, uint32_t seq) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;

  String url = String("http://") + PLUGIN_HOST + ":" + String(PLUGIN_PORT) + "/event";

  // Always includes "event" for compatibility
  String body = "{";
  body += "\"event\":" + String(eventNumber);
  body += ",\"seq\":" + String(seq);
  body += ",\"deviceId\":\"" + deviceId + "\"";
  body += ",\"key\":" + String(keyIndex);
  body += ",\"press\":\"" + pressKind + "\"";
  body += ",\"heldMs\":" + String(heldMs);
  body += ",\"uptimeMs\":" + String(millis());
  if (WiFi.status() == WL_CONNECTED) body += ",\"rssi\":" + String(WiFi.RSSI());
  body += "}";

  uint32_t backoff = 200;
  for (uint8_t attempt = 1; attempt <= 3; attempt++) {
    http.setTimeout(1200);

    if (!http.begin(client, url)) {
      http.end();
      delay(backoff); backoff *= 2;
      continue;
    }

    http.addHeader("Content-Type", "application/json");
    if (PLUGIN_SECRET && PLUGIN_SECRET[0] != '\0') {
      http.addHeader("X-Matrix-Secret", PLUGIN_SECRET);
    }

    int code = http.POST((uint8_t*)body.c_str(), body.length());
    String resp = http.getString();
    http.end();

    if (code >= 200 && code < 300) {
      // If plugin supports seq ACK, record it; otherwise assume accepted
      uint32_t ack;
      if (parseAckSeq(resp, ack) && ack == seq) lastAckSeq = ack;
      else lastAckSeq = seq;
      return true;
    }

    delay(backoff);
    backoff *= 2;
    yield();
  }

  return false;
}

// ===================== SETUP / LOOP =====================

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  ledOff();

  // Serial RX is Pico; avoid Serial prints
  Serial.begin(PICO_BAUD);
  Serial.setDebugOutput(false);
  delay(150);

  deviceId = "lumicon-" + String(ESP.getChipId(), HEX);

  // TFT init
  SPI.begin();
  tft.initR(ST7735_TAB);
  tft.setRotation(1);
  tft.fillScreen(BG);
  tft.setTextColor(FG, BG);
  tft.setTextSize(1);

  statusBase = "Boot...";
  redrawScreen();

  // Factory reset window
  statusBase = "Hold K0=Reset";
  redrawScreen();

  if (checkFactoryResetHold()) {
    statusBase = "Resetting WiFi";
    redrawScreen();
    blink(5, 80, 80);

    WiFiManager wm;
    wm.setDebugOutput(false);
    wm.resetSettings();
    delay(200);
    ESP.restart();
  }

  // WiFiManager connect
  statusBase = "WiFi setup...";
  redrawScreen();

  WiFiManager wm;
  wm.setDebugOutput(false);
  wm.setConfigPortalTimeout(180);
  wm.autoConnect("Lumi-Con-Setup");

  // WiFi OK: 2 quick blinks
  blink(2, 50, 100);

  statusBase = "IP:" + WiFi.localIP().toString();
  redrawScreen();

  // PC reachable: 3 quick, fail: 3 slow
  if (checkPcReachable()) blink(3, 50, 100);
  else blink(3, 200, 150);

  // Routes
  server.on("/", handleRoot);
  server.on("/msg", HTTP_GET, handleMsg);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/clear", HTTP_GET, handleClear);
  server.on("/ui", HTTP_POST, handleUi);
  server.on("/health", HTTP_GET, handleHealth);
  server.begin();
}

void loop() {
  server.handleClient();

  uint8_t type, key;
  if (readPicoPacket(type, key)) {
    if (!picoSeen) { picoSeen = true; blink(1, 40, 80); } // Pico detected once
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
        bool isLong = held >= LONG_PRESS_MS;

        uint8_t eventOut = isLong ? (uint8_t)(key + 36) : key;
        String pressKind = isLong ? "long" : "short";

        // Show last key pressed on TFT
        lastKeyText = "K:" + String(key) + (isLong ? "L" : "S");
        redrawScreen();

        // seq increment
        seqCounter++;
        uint32_t seq = seqCounter;
        lastSeqSent = seq;

        bool ok = postEventToPlugin(eventOut, key, pressKind, held, seq);
        lastPostOk = ok;

        // LED feedback for HTTP result
        if (ok) blink(1, 30, 60);
        else blink(2, 30, 60);
      }
    }
  }

  yield();
}