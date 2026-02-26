/*
  Lumi-Con ESP8266 firmware v0.0.3 (amended)
  -----------------------------------------
  Changes in this amendment:
  - Header is now TWO lines:
      Line 1: transient/prompt ("ACK OK", "ACK FAIL", "1=LEG 2=ACK", etc.)
      Line 2: base status + last key (e.g. "IP:192.168.1.50 K:12L")
  - Transient message expiry triggers an immediate redraw.
  - LED blinks on EVERY PRESS packet (helps diagnose Pico/UART).

  Existing:
  - After WiFi connects and IP is shown, ESP waits for mode choice from Pico matrix:
      Key 1 => LEGACY
      Key 2 => CONFIRMED
    No auto mode. Waits indefinitely.

  Endpoints unchanged:
    GET  /msg?t=...
    GET  /status?t=...
    GET  /clear
    POST /ui {"channel":"chat|status|clear","text":"..."}
    GET  /health
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

// ---- Lumia plugin endpoint (PC running Lumia) ----
const char* PLUGIN_HOST = "192.168.1.87";
constexpr uint16_t PLUGIN_PORT = 8787;
const char* PLUGIN_SECRET = ""; // optional header X-Matrix-Secret

// ---- Device identity ----
static String deviceId;

// ---- Pico UART ----
constexpr uint32_t PICO_BAUD = 115200;
constexpr uint8_t KEY_COUNT = 36;
constexpr uint32_t LONG_PRESS_MS = 600;

// ---- Mode selection keys (from Pico matrix) ----
constexpr uint8_t MODE_KEY_LEGACY = 1;    // press Key 1
constexpr uint8_t MODE_KEY_CONFIRM = 2;   // press Key 2

// ---- Factory reset trigger ----
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

// Header is now 2 lines
static const int STATUS_LINES = 2;
static const int CHAT_LINES = MAX_LINES - STATUS_LINES;

// ===================== GLOBALS =====================

ESP8266WebServer server(80);

String chatBuf[CHAT_LINES];
int chatCount = 0;

// Line 2 base status (IP, etc.)
String statusBase = "";

// last key (shown on line 2)
String lastKeyText = "K:--";

// Line 1 transient/prompt/status
String transientStatus = "";
uint32_t transientUntilMs = 0;

uint32_t pressStart[KEY_COUNT] = {};
bool isDown[KEY_COUNT] = {};

bool picoSeen = false;

uint32_t seqCounter = 0;
uint32_t lastSeqSent = 0;
uint32_t lastAckSeq = 0;
bool lastPostOk = false;

enum ReliabilityMode { MODE_LEGACY, MODE_CONFIRMED };
ReliabilityMode reliabilityMode = MODE_LEGACY;

// ===================== LED HELPERS (ESP8266 LED is active-low) =====================

void ledOn()  { digitalWrite(LED_BUILTIN, LOW); }
void ledOff() { digitalWrite(LED_BUILTIN, HIGH); }

void blink(uint8_t times, uint16_t onMs, uint16_t offMs) {
  for (uint8_t i = 0; i < times; i++) {
    ledOn();  delay(onMs);
    ledOff(); delay(offMs);
    yield();
  }
}

// Quick “tick blink” for press feedback (very short so it doesn't feel laggy)
void blinkPressTick() {
  ledOn(); delay(12);
  ledOff(); delay(8);
}

// ===================== TRANSIENT STATUS HELPERS =====================

void setTransientStatus(const String& msg, uint32_t ttlMs) {
  transientStatus = msg;
  transientUntilMs = millis() + ttlMs;
  // immediate redraw to show it now
  // (we call redrawScreen() where this is set to avoid extra redraws)
}

void updateTransientStatus() {
  if (!transientStatus.length()) return;

  if ((int32_t)(millis() - transientUntilMs) >= 0) {
    transientStatus = "";
    redrawScreen(); // <-- requested: redraw immediately when it expires
  }
}

// ===================== STRING / TFT HELPERS =====================

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

String truncateCols(const String& s) {
  if ((int)s.length() <= MAX_COLS) return s;
  return s.substring(0, MAX_COLS);
}

void addChatLine(const String& line) {
  if (chatCount < CHAT_LINES) chatBuf[chatCount++] = line;
  else {
    for (int i = 1; i < CHAT_LINES; i++) chatBuf[i - 1] = chatBuf[i];
    chatBuf[CHAT_LINES - 1] = line;
  }
}

void redrawScreen() {
  tft.fillScreen(BG);
  tft.setTextWrap(false);
  tft.setTextSize(1);
  tft.setTextColor(FG, BG);

  // -------- Line 1 (top): transient/prompt/status --------
  String line1 = transientStatus.length() ? transientStatus : "";
  line1 = truncateCols(line1);
  tft.setCursor(0, 0);
  tft.print(line1);

  // -------- Line 2: base status + last key --------
  String line2 = statusBase;
  if (line2.length()) line2 += " ";
  line2 += lastKeyText;
  line2 = truncateCols(line2);

  tft.setCursor(0, LINE_H);
  tft.print(line2);

  // -------- Chat area (below header) bottom-aligned --------
  int visible = CHAT_LINES;
  int startIndex = (chatCount > visible) ? (chatCount - visible) : 0;
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

void clearAll() {
  chatCount = 0;
  statusBase = "";
  transientStatus = "";
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

  String modeStr = (reliabilityMode == MODE_CONFIRMED) ? "confirmed" : "legacy";

  String json = "{";
  json += "\"ok\":true";
  json += ",\"deviceId\":\"" + deviceId + "\"";
  json += ",\"mode\":\"" + modeStr + "\"";
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

// ===================== RELIABILITY MODE SELECTION (BLOCKING) =====================

void waitForModeSelection() {
  transientStatus = "1=LEG 2=ACK";   // line 1
  redrawScreen();

  uint32_t lastBlink = millis();
  while (true) {
    if (millis() - lastBlink > 1500) {
      blink(1, 20, 20);
      lastBlink = millis();
    }

    uint8_t type, key;
    if (readPicoPacket(type, key)) {
      if (!picoSeen) { picoSeen = true; blink(1, 40, 80); }

      if (type == 1) { // PRESS
        blinkPressTick(); // <-- requested: blink each press to prove Pico is alive

        if (key == MODE_KEY_LEGACY) {
          reliabilityMode = MODE_LEGACY;
          transientStatus = "MODE:LEGACY";
          redrawScreen();
          blink(2, 40, 80);
          delay(600);
          transientStatus = ""; // clear prompt after selection
          redrawScreen();
          return;
        }

        if (key == MODE_KEY_CONFIRM) {
          reliabilityMode = MODE_CONFIRMED;
          transientStatus = "MODE:CONFIRM";
          redrawScreen();
          blink(3, 40, 80);
          delay(600);
          transientStatus = "";
          redrawScreen();
          return;
        }
      }
    }

    yield();
  }
}

// ===================== POST EVENT + SEQ ACK =====================

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
      if (reliabilityMode == MODE_LEGACY) {
        lastAckSeq = seq;
        return true;
      }

      uint32_t ack;
      if (parseAckSeq(resp, ack) && ack == seq) {
        lastAckSeq = ack;
        return true;
      }

      // 2xx but missing/wrong ack => failure in CONFIRMED
      delay(backoff);
      backoff *= 2;
      continue;
    }

    delay(backoff);
    backoff *= 2;
    yield();
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
      if (!picoSeen) { picoSeen = true; blink(1, 40, 80); }

      if (type == 1) {
        blinkPressTick(); // show Pico activity during reset window too
      }

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

// ===================== SETUP / LOOP =====================

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  ledOff();

  Serial.begin(PICO_BAUD);
  Serial.setDebugOutput(false);
  delay(150);

  deviceId = "lumicon-" + String(ESP.getChipId(), HEX);

  SPI.begin();
  tft.initR(ST7735_TAB);
  tft.setRotation(1);
  tft.fillScreen(BG);
  tft.setTextColor(FG, BG);
  tft.setTextSize(1);

  // Show boot info (line1 empty, line2 shows statusBase)
  transientStatus = "";
  statusBase = "Boot...";
  redrawScreen();

  transientStatus = "Hold K0=Reset";
  redrawScreen();

  if (checkFactoryResetHold()) {
    transientStatus = "Resetting WiFi";
    redrawScreen();
    blink(5, 80, 80);

    WiFiManager wm;
    wm.setDebugOutput(false);
    wm.resetSettings();
    delay(200);
    ESP.restart();
  }

  transientStatus = "WiFi setup...";
  redrawScreen();

  WiFiManager wm;
  wm.setDebugOutput(false);
  wm.setConfigPortalTimeout(180);
  wm.autoConnect("Lumi-Con-Setup");

  blink(2, 50, 100);

  // IMPORTANT: IP is now on LINE 2 (statusBase)
  statusBase = "IP:" + WiFi.localIP().toString();
  transientStatus = ""; // keep line1 clear until prompt
  redrawScreen();

  // WAIT FOR MODE SELECTION (prompt shown on line 1, IP stays on line 2)
  waitForModeSelection();

  // HTTP routes
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

  // Expire transient messages (and redraw immediately when they expire)
  updateTransientStatus();

  uint8_t type, key;
  if (readPicoPacket(type, key)) {
    if (!picoSeen) { picoSeen = true; blink(1, 40, 80); }

    // LED blink on every PRESS packet (requested)
    if (type == 1) {
      blinkPressTick();
    }

    if (key >= KEY_COUNT) return;

    uint32_t now = millis();

    if (type == 1) {
      if (!isDown[key]) {
        isDown[key] = true;
        pressStart[key] = now;
      }
    } else {
      if (isDown[key]) {
        isDown[key] = false;

        uint32_t held = now - pressStart[key];
        bool isLong = held >= LONG_PRESS_MS;

        uint8_t eventOut = isLong ? (uint8_t)(key + 36) : key;
        String pressKind = isLong ? "long" : "short";

        lastKeyText = "K:" + String(key) + (isLong ? "L" : "S");
        redrawScreen();

        seqCounter++;
        uint32_t seq = seqCounter;
        lastSeqSent = seq;

        bool ok = postEventToPlugin(eventOut, key, pressKind, held, seq);
        lastPostOk = ok;

        if (ok) {
          blink(1, 30, 60);
          setTransientStatus("ACK OK", 2000);
        } else {
          blink(2, 30, 60);
          setTransientStatus("ACK FAIL", 3000);
        }

        redrawScreen();
      }
    }
  }

  yield();
}