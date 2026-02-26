/*
  Lumi-Con ESP8266 firmware v0.0.1 (integrated)
  -------------------------------------
  - TFT display (ST7735 1.8" 128x160)
  - Receives display text via HTTP:
      GET  /msg?t=Hello%20world      -> chat/log area (wrapped, scrolling)
      GET  /status?t=OK%3A%20Done    -> status/confirm line (top line)
      GET  /clear                   -> clears screen/buffers
      POST /ui  {"channel":"chat|status|clear","text":"..."}  -> future UI hook

  - Receives key press/release packets from Pico over UART (ESP Serial RX):
      Packet: A5, type(1 press / 0 release), key(0..35), xor

  - Computes short/long press and sends to Lumia plugin:
      POST http://PLUGIN_HOST:PLUGIN_PORT/event
      Body: {"event": N}   where:
        short press: 0..35
        long press:  36..71  (key + 36)

  Notes:
  - WiFiManager is used, but Serial output is disabled to avoid corrupting UART RX.
  - Firmware is future-ready: status line reserved, /ui JSON endpoint included.
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

// Optional shared secret (must match plugin setting), leave "" to disable
const char* PLUGIN_SECRET = "";

// ---- Pico UART ----
constexpr uint32_t PICO_BAUD = 115200;
constexpr uint8_t KEY_COUNT = 36;
constexpr uint32_t LONG_PRESS_MS = 600;

// ---- TFT pins (keep your existing wiring) ----
#define TFT_CS   D2
#define TFT_DC   D1
#define TFT_RST  D0   // If you hit white-screen issues, see notes at bottom.

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

// For many 1.8" ST7735 128x160 modules:
// INITR_GREENTAB / INITR_REDTAB / INITR_BLACKTAB
#define ST7735_TAB INITR_GREENTAB

// ---- Screen geometry ----
static const uint16_t BG = ST77XX_BLACK;
static const uint16_t FG = ST77XX_WHITE;

static const int SCREEN_W = 160;
static const int SCREEN_H = 128;

static const int CHAR_W = 6;  // default font 5+1
static const int LINE_H = 8;
static const int MAX_COLS = SCREEN_W / CHAR_W;   // ~26 for 160/6
static const int MAX_LINES = SCREEN_H / LINE_H;  // 16 for 128/8

// Reserve 1 line for status (future screen partition hook)
static const int STATUS_LINES = 1;
static const int CHAT_LINES = MAX_LINES - STATUS_LINES;

// ===================== GLOBALS =====================

ESP8266WebServer server(80);

// Chat ring buffer
String chatBuf[CHAT_LINES];
int chatCount = 0;

// Status line
String statusLine = "";

// Key press tracking for long press calc (ESP-side)
uint32_t pressStart[KEY_COUNT] = {};
bool isDown[KEY_COUNT] = {};

// ===================== UTILITIES =====================

static inline String trimRightSpaces(String s) {
  while (s.endsWith(" ")) s.remove(s.length() - 1);
  return s;
}

String urlDecode(const String& in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < in.length()) {
      char h1 = in[i + 1];
      char h2 = in[i + 2];
      auto hexVal = [](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return 10 + (h - 'a');
        if (h >= 'A' && h <= 'F') return 10 + (h - 'A');
        return 0;
      };
      char decoded = (hexVal(h1) << 4) | hexVal(h2);
      out += decoded;
      i += 2;
    } else {
      out += c;
    }
  }
  return out;
}

String truncateToCols(const String& s, int cols) {
  if ((int)s.length() <= cols) return s;
  return s.substring(0, cols);
}

// ===================== TFT RENDERING =====================

void addChatLine(const String& line) {
  if (chatCount < CHAT_LINES) {
    chatBuf[chatCount++] = line;
  } else {
    // shift up
    for (int i = 1; i < CHAT_LINES; i++) chatBuf[i - 1] = chatBuf[i];
    chatBuf[CHAT_LINES - 1] = line;
  }
}

void redrawScreen() {
  tft.fillScreen(BG);
  tft.setTextWrap(false);
  tft.setTextSize(1);
  tft.setTextColor(FG, BG);

  int y = 0;

  // Status line (top line)
  String st = truncateToCols(statusLine, MAX_COLS);
  if (st.length()) {
    tft.setCursor(0, y);
    tft.print(st);
  }
  y += LINE_H * STATUS_LINES;

  // Chat area bottom-aligned within remaining space (like your original)
  int visible = CHAT_LINES;
  int startIndex = (chatCount > visible) ? (chatCount - visible) : 0;

  // Compute where to start so it's bottom-up within the chat region
  int linesToDraw = chatCount - startIndex;
  int chatRegionH = (MAX_LINES - STATUS_LINES) * LINE_H;
  int yStart = (STATUS_LINES * LINE_H) + (chatRegionH - linesToDraw * LINE_H);
  if (yStart < STATUS_LINES * LINE_H) yStart = STATUS_LINES * LINE_H;

  int yy = yStart;
  for (int i = startIndex; i < chatCount; i++) {
    tft.setCursor(0, yy);
    tft.print(truncateToCols(chatBuf[i], MAX_COLS));
    yy += LINE_H;
    if (yy >= SCREEN_H) break;
  }
}

// Wrap + add to chat buffer (your original behavior)
void pushWrappedChat(const String& msgRaw) {
  String msg = msgRaw;
  msg.replace("\r", "");

  int start = 0;
  while (start <= (int)msg.length()) {
    int nl = msg.indexOf('\n', start);
    String segment = (nl == -1) ? msg.substring(start) : msg.substring(start, nl);
    segment = trimRightSpaces(segment);

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

void clearAll() {
  chatCount = 0;
  statusLine = "";
  tft.fillScreen(BG);
}

// ===================== HTTP HANDLERS =====================

void handleRoot() {
  String page =
    "Lumi-Con ESP OK\n\n"
    "Chat/log:\n"
    "  /msg?t=Hello%20world\n\n"
    "Status:\n"
    "  /status?t=OK%3A%20Lights%20On\n\n"
    "Clear:\n"
    "  /clear\n\n"
    "Future UI:\n"
    "  POST /ui {\"channel\":\"chat|status|clear\",\"text\":\"...\"}\n";
  server.send(200, "text/plain", page);
}

void handleMsg() {
  if (!server.hasArg("t")) {
    server.send(400, "text/plain", "Missing query param 't'");
    return;
  }
  String text = urlDecode(server.arg("t"));
  if (!text.length()) {
    server.send(400, "text/plain", "Empty message");
    return;
  }
  pushWrappedChat(text);
  server.send(200, "text/plain", "OK");
}

void handleStatus() {
  if (!server.hasArg("t")) {
    server.send(400, "text/plain", "Missing query param 't'");
    return;
  }
  String text = urlDecode(server.arg("t"));
  statusLine = text;
  redrawScreen();
  server.send(200, "text/plain", "OK");
}

void handleClear() {
  clearAll();
  server.send(200, "text/plain", "CLEARED");
}

// Minimal JSON extraction without adding heavy libs
// Expects: {"channel":"chat|status|clear","text":"..."}
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
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "POST only");
    return;
  }

  String body = server.arg("plain");
  body.trim();
  if (!body.length()) {
    server.send(400, "text/plain", "Empty body");
    return;
  }

  String channel = jsonFindString(body, "channel");
  String text = jsonFindString(body, "text");

  if (channel == "clear") {
    clearAll();
    server.send(200, "text/plain", "OK");
    return;
  }

  if (!text.length()) {
    server.send(400, "text/plain", "Missing text");
    return;
  }

  if (channel == "status") {
    statusLine = text;
    redrawScreen();
  } else {
    // default to chat
    pushWrappedChat(text);
  }

  server.send(200, "text/plain", "OK");
}

// ===================== PICO UART PACKET DECODE =====================

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

// ===================== POST EVENT TO PLUGIN =====================

bool postEventToPlugin(uint8_t eventNumber) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;

  String url = String("http://") + PLUGIN_HOST + ":" + String(PLUGIN_PORT) + "/event";
  String body = String("{\"event\":") + String(eventNumber) + "}";

  uint32_t backoff = 200;
  for (uint8_t attempt = 1; attempt <= 3; attempt++) {
    http.setTimeout(1200);

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

// ===================== SETUP / LOOP =====================

void showBootLine(const String& s) {
  statusLine = s;
  redrawScreen();
}

void setup() {
  // IMPORTANT: Serial RX is used for Pico packets. Avoid Serial prints entirely.
  Serial.begin(PICO_BAUD);
  Serial.setDebugOutput(false);
  delay(150);

  // TFT init (do this early)
  SPI.begin(); // safe explicit init
  tft.initR(ST7735_TAB);
  tft.setRotation(1);
  tft.fillScreen(BG);
  tft.setTextColor(FG, BG);
  tft.setTextSize(1);

  showBootLine("WiFi setup...");

  // WiFiManager (NO DEBUG OUTPUT to Serial!)
  WiFiManager wm;
  wm.setDebugOutput(false);
  wm.setConfigPortalTimeout(180);

  // Optional: show AP name if config portal starts
  wm.setAPCallback([](WiFiManager *wmp) {
    (void)wmp;
    // Can't capture safely here; keep minimal.
  });

  bool ok = wm.autoConnect("Lumi-Con-Setup");
  (void)ok;

  showBootLine("IP: " + WiFi.localIP().toString());

  // HTTP routes
  server.on("/", handleRoot);
  server.on("/msg", HTTP_GET, handleMsg);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/clear", HTTP_GET, handleClear);
  server.on("/ui", HTTP_POST, handleUi);
  server.begin();

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
}

void loop() {
  server.handleClient();

  // Pico UART -> short/long -> plugin
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

        bool okSend = postEventToPlugin(eventOut);

        // Tiny LED feedback only (no Serial)
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