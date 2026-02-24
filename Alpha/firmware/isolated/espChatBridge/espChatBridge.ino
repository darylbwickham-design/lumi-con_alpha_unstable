/*
  ESP8266 + ST7735 1.8" 128x160
  Receives messages from Lumia plugin via:
    http://ESP_IP/msg?t=Hello%20world

  Displays text bottom-up, wraps long lines, scrolls upward,
  rolls off top when full.
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

// ----------- WIFI -----------
const char* WIFI_SSID = "SSID";
const char* WIFI_PASS = "PASS";

// ----------- TFT PINS -----------
// Common wiring (adjust to your setup):
// ESP8266 (NodeMCU/Wemos D1 mini) hardware SPI:
//   SCK = D5 (GPIO14)
//   MOSI= D7 (GPIO13)
//   MISO not used
// Choose CS/DC/RST pins:
#define TFT_CS   D2
#define TFT_DC   D1
#define TFT_RST  D0

// ----------- TFT SETUP -----------
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

// For many 1.8" ST7735 128x160 modules:
// - INITR_BLACKTAB, INITR_REDTAB, INITR_GREENTAB
// If colors look wrong, try a different tab.
#define ST7735_TAB INITR_GREENTAB

// ----------- SERVER -----------
ESP8266WebServer server(80);

// ----------- TEXT LAYOUT -----------
static const uint16_t BG = ST77XX_BLACK;
static const uint16_t FG = ST77XX_WHITE;

static const int SCREEN_W = 160;
static const int SCREEN_H = 128;

static const int CHAR_W = 6;   // Adafruit_GFX default font: 5px + 1px spacing
static const int CHAR_H = 8;   // default font height
static const int LINE_H = 8;   // keep equal to CHAR_H for simple layout
static const int MAX_COLS = SCREEN_W / CHAR_W;   // ~21
static const int MAX_LINES = SCREEN_H / LINE_H;  // 20

// A simple ring buffer of lines
String lineBuf[MAX_LINES];
int lineCount = 0;       // how many valid lines currently in buffer (<= MAX_LINES)

// --- Utilities ---
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

// Break a string into wrapped lines (word-wrap-ish)
void pushWrappedMessage(const String& msgRaw) {
  String msg = msgRaw;
  msg.replace("\r", "");
  // Support explicit newlines in the incoming message:
  // we’ll split by '\n' first, then wrap each segment.
  int start = 0;
  while (start <= (int)msg.length()) {
    int nl = msg.indexOf('\n', start);
    String segment = (nl == -1) ? msg.substring(start) : msg.substring(start, nl);

    // Trim only right side a bit (optional)
    while (segment.endsWith(" ")) segment.remove(segment.length() - 1);

    // Wrap this segment
    int idx = 0;
    while (idx < (int)segment.length()) {
      int remaining = segment.length() - idx;

      // If remaining fits, take it
      if (remaining <= MAX_COLS) {
        addLine(segment.substring(idx));
        idx = segment.length();
        break;
      }

      // Otherwise, find last space within MAX_COLS
      int cut = idx + MAX_COLS;
      int space = segment.lastIndexOf(' ', cut);
      if (space <= idx) {
        // no space found, hard cut
        addLine(segment.substring(idx, idx + MAX_COLS));
        idx += MAX_COLS;
      } else {
        addLine(segment.substring(idx, space));
        idx = space + 1; // skip space
      }
    }

    if (nl == -1) break;
    // If there was a newline, preserve it as a blank line gap (optional):
    // addLine("");  // uncomment if you want an empty line between newline-separated blocks
    start = nl + 1;
  }

  redrawScreen();
}

void addLine(const String& line) {
  if (lineCount < MAX_LINES) {
    lineBuf[lineCount++] = line;
  } else {
    // shift up
    for (int i = 1; i < MAX_LINES; i++) {
      lineBuf[i - 1] = lineBuf[i];
    }
    lineBuf[MAX_LINES - 1] = line;
  }
}

void redrawScreen() {
  tft.fillScreen(BG);
  tft.setTextWrap(false); // we do wrapping ourselves
  tft.setTextSize(1);
  tft.setTextColor(FG, BG);

  int yStart = SCREEN_H - (lineCount * LINE_H);
  if (yStart < 0) yStart = 0;

  for (int i = 0; i < lineCount; i++) {
    int y = yStart + i * LINE_H;
    tft.setCursor(0, y);
    tft.print(lineBuf[i]);
  }
}

// --- HTTP handlers ---
void handleRoot() {
  String page =
    "ESP8266 ST7735 Display OK\n\n"
    "Send a message:\n"
    "/msg?t=Hello%20world\n";
  server.send(200, "text/plain", page);
}

void handleMsg() {
  if (!server.hasArg("t")) {
    server.send(400, "text/plain", "Missing query param 't'");
    return;
  }
  String text = urlDecode(server.arg("t"));
  if (text.length() == 0) {
    server.send(400, "text/plain", "Empty message");
    return;
  }

  pushWrappedMessage(text);
  server.send(200, "text/plain", "OK");
}

void handleClear() {
  lineCount = 0;
  tft.fillScreen(BG);
  server.send(200, "text/plain", "CLEARED");
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // TFT init
  tft.initR(ST7735_TAB);
  tft.setRotation(1); // 0/1/2/3 - try 1 for landscape, 0 for portrait
  tft.fillScreen(BG);
  tft.setTextColor(FG, BG);
  tft.setTextSize(1);
  tft.setCursor(0, 0);
  tft.print("WiFi...");

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }

  tft.fillScreen(BG);
  tft.setCursor(0, 0);
  tft.print("IP:");
  tft.setCursor(0, 10);
  tft.print(WiFi.localIP().toString());

  Serial.println();
  Serial.print("Connected. IP: ");
  Serial.println(WiFi.localIP());

  // Server routes
  server.on("/", handleRoot);
  server.on("/msg", HTTP_GET, handleMsg);
  server.on("/clear", HTTP_GET, handleClear);

  server.begin();
}

void loop() {
  server.handleClient();
}