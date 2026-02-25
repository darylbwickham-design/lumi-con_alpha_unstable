# Lumi-Con (ESP8266 + Pico) — Lumia-Based Stream Controller

Lumi-Con is an open-source, Lumia-first “stream deck” style controller built around:
- **Raspberry Pi Pico (RP2040)**: scans a key matrix (currently 6×6 test matrix)
- **ESP8266**: Wi-Fi + HTTP + TFT display + relays key events to Lumia via a plugin

This repo/docs cover **three supported builds**:

1) **6×6 ONLY (Matrix Relay)**  
   Pico → ESP → Lumia plugin (alerts/variations)

2) **CHAT RELAY ONLY (TFT Chat Bridge)**  
   Lumia plugin → ESP TFT (display messages)

3) **INTEGRATED (Recommended)**  
   One ESP firmware + one plugin (events + TFT actions)

---

## Table of Contents
- [Hardware Overview](#hardware-overview)
- [Arduino IDE One-Time Setup](#arduino-ide-one-time-setup)
- [Wiring](#wiring)
- [Firmware Upload](#firmware-upload)
  - [Flash the Pico (6×6 scanner)](#flash-the-pico-66-scanner)
  - [Flash the ESP8266](#flash-the-esp8266)
- [Plugin Build + Install](#plugin-build--install)
- [Build 1: 6×6 ONLY (Matrix Relay)](#build-1-66-only-matrix-relay)
- [Build 2: CHAT RELAY ONLY (TFT Chat Bridge)](#build-2-chat-relay-only-tft-chat-bridge)
- [Build 3: INTEGRATED (Recommended)](#build-3-integrated-recommended)
- [Plugin Icon](#plugin-icon)
- [Troubleshooting](#troubleshooting)

---

## Hardware Overview

### Current test configuration
- **Pico** scans a **6×6** keypad matrix and sends packets over UART to the ESP8266.
- **ESP8266**:
  - receives UART packets on RX
  - computes short/long press
  - `POST`s key events to the Lumia plugin `/event`
  - hosts TFT endpoints:
    - `GET /msg?t=...`
    - `GET /status?t=...`
    - `GET /clear`
    - optional: `POST /ui` (future UI hook)

---

## Arduino IDE One-Time Setup

### 1) Install Arduino IDE
https://www.arduino.cc/en/software

### 2) Add board manager URLs
Arduino IDE → **File → Preferences** → **Additional Boards Manager URLs**  
Paste (comma-separated is fine):

- ESP8266:
  - `http://arduino.esp8266.com/stable/package_esp8266com_index.json`
- Pico (Earle Philhower):
  - `https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json`

### 3) Install board packages
Arduino IDE → **Tools → Board → Boards Manager**  
Install:
- **ESP8266 by ESP8266 Community**
- **Raspberry Pi Pico/RP2040** (Earle Philhower)

---

## Wiring

> **IMPORTANT (ESP flashing):** When uploading to the ESP8266, disconnect **Pico TX (GP0) → ESP RX (GPIO3)** or the upload may fail.

### A) Pico ↔ 6×6 Matrix (matches `6x6_test_pico.ino`)
**Rows** (inputs w/ pullups):
- R0 → **GP12**
- R1 → **GP1**
- R2 → **GP2**
- R3 → **GP3**
- R4 → **GP4**
- R5 → **GP5**

**Cols** (outputs):
- C0 → **GP6**
- C1 → **GP7**
- C2 → **GP8**
- C3 → **GP9**
- C4 → **GP10**
- C5 → **GP11**

### B) Pico → ESP8266 UART
- Pico **GP0 (TX)** → ESP **RX / GPIO3**
- Pico **GND** → ESP **GND**
- (Optional but recommended) **1k resistor** in series on the TX line

### C) ESP8266 → ST7735 TFT (SPI)
Common wiring for NodeMCU / Wemos D1 mini style ESP8266:

- TFT **VCC**  → **3V3**
- TFT **GND**  → **GND**
- TFT **SCK**  → **D5 (GPIO14)**
- TFT **MOSI** → **D7 (GPIO13)**
- TFT **CS**   → **D2 (GPIO4)**
- TFT **DC**   → **D1 (GPIO5)**
- TFT **RST**  → **D0 (GPIO16)**

**If the TFT is white:** see [Troubleshooting](#troubleshooting).

---

## Firmware Upload

### Flash the Pico (6×6 scanner)
1. Plug Pico into your PC via USB
2. Arduino IDE → select board: **Raspberry Pi Pico**
3. Open: `6x6_test_pico.ino`
4. Upload
5. Optional: Serial Monitor at **115200** to see `PRESS/RELEASE` logs

### Flash the ESP8266
1. **Disconnect** Pico GP0 → ESP RX (GPIO3) while uploading
2. Plug ESP8266 into PC via USB
3. Arduino IDE → select your ESP8266 board (NodeMCU / Wemos D1 mini / etc.)
4. Open and upload the ESP sketch you want (see build sections below)
5. Reconnect Pico GP0 → ESP RX after upload

---

## Plugin Build + Install

### Build (CLI)
Open a terminal in the plugin folder (the folder containing `package.json`):

PLUGIN BUILD (WINDOWS)

1) Open CMD/PowerShell in the plugin folder (the folder with package.json)
2) Run:

npm install
npx lumia-plugin validate .
npx lumia-plugin build . --out .\OUTPUT_NAME.lumiaplugin

BUILD 1 — 6x6 ONLY (Matrix Relay)

PLUGIN (Lumia):
- Install: matrix_relay_6x6_ver.lumiaplugin
- Settings:
  Enable Listener = ON
  Listen Port = 8787
  Shared Secret = (blank)

ESP (edit + flash):
const char* PLUGIN_HOST = "192.168.1.87"; // your PC IP
constexpr uint16_t PLUGIN_PORT = 8787;    // same as Listen Port
const char* PLUGIN_SECRET = "";           // same as Shared Secret (or blank)

BUILD COMMAND (in plugin folder):
npm install
npx lumia-plugin validate .
npx lumia-plugin build . --out .\matrix_relay_6x6_ver.lumiaplugin

BUILD 2 — CHAT RELAY ONLY (TFT Chat Bridge)

ESP (edit + flash espChatBridge.ino):
const char* WIFI_SSID = "YourWiFiName";
const char* WIFI_PASS = "YourWiFiPassword";

PLUGIN (Lumia):
- Install: ESP8266_TFT_Chat_Bridge.lumiaplugin
- Settings:
  ESP Base URL = http://<ESP_IP>
  Message Path = /msg
  Clear Path = /clear

QUICK TEST (browser):
http://<ESP_IP>/msg?t=Hello%20world
http://<ESP_IP>/clear

BUILD COMMAND (in plugin folder):
npm install
npx lumia-plugin validate .
npx lumia-plugin build . --out .\ESP8266_TFT_Chat_Bridge.lumiaplugin

BUILD 3 — INTEGRATED (One ESP + One Plugin)

ESP (first-time WiFi):
1) Power ESP -> WiFi AP appears: Lumi-Con-Setup
2) Connect to Lumi-Con-Setup
3) Open: http://192.168.4.1
4) Select home WiFi + password -> Save
5) ESP reboots -> TFT shows IP: x.x.x.x

PLUGIN (Lumia):
- Install: lumi_con_bridge_integrated.lumiaplugin (NEW ID)
- Settings:
  Enable Listener = ON
  Listen Port = 8787
  Shared Secret = (blank)
  ESP Base URL = http://<ESP_IP>
  UI Mode = legacy_get

ESP (edit + flash integrated firmware):
const char* PLUGIN_HOST = "192.168.1.87"; // your PC IP
constexpr uint16_t PLUGIN_PORT = 8787;    // same as Listen Port
const char* PLUGIN_SECRET = "";           // same as Shared Secret (or blank)

BUILD COMMAND (in plugin folder):
npm install
npx lumia-plugin validate .
npx lumia-plugin build . --out .\lumi_con_bridge_integrated.lumiaplugin

---

## PLUGIN ICON (ADD TO ANY PLUGIN)

1) Put your icon file in the plugin folder (same folder as manifest.json)
   Recommended name: icon.png

   Suggested icon file (tight, readable):
   lumi-con-icon-tight-256.png  -> rename to: icon.png

2) Edit manifest.json (TOP LEVEL, near id/name/version) and add:

"icon": "./icon.png",

Example:

{
  "id": "your_plugin_id",
  "name": "Your Plugin Name",
  "version": "1.0.0",
  "icon": "./icon.png",
  ...
}

3) Rebuild the plugin:

npm install
npx lumia-plugin validate .
npx lumia-plugin build . --out .\YOUR_OUTPUT_NAME.lumiaplugin

NOTES
- The icon must be included in the plugin folder before you build.
- PNG square icons work best (256x256 recommended).
- If validate fails, double-check the icon path matches the filename exactly.

---
