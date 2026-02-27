# Lumi-Con (ESP8266 + Pico) — Lumia-Based Stream Controller

Lumi-Con is an open-source, Lumia-first **control deck** built around:
- **Raspberry Pi Pico (RP2040)**: scans a key matrix (currently 6×6 test matrix)
- **ESP8266**: Wi-Fi + HTTP + TFT display + relays key events to **Lumia** via a plugin

This repo/docs cover **three supported builds**:

1) **6×6 ONLY (Matrix Relay)** — Pico → ESP → Lumia plugin (alerts/variations)  
2) **CHAT RELAY ONLY (TFT Chat Bridge)** — Lumia plugin → ESP TFT (display messages)  
3) **INTEGRATED (Recommended / Pro)** — one ESP firmware + one integrated plugin (events + TFT actions + device status)

---

## Table of Contents
- [Hardware Overview](#hardware-overview)
- [Arduino IDE One-Time Setup](#arduino-ide-one-time-setup)
- [Wiring](#wiring)
- [Firmware Upload](#firmware-upload)
- [Plugin Build + Install (CLI)](#plugin-build--install-cli)
- [Build 1: 6×6 ONLY (Matrix Relay)](#build-1-66-only-matrix-relay)
- [Build 2: CHAT RELAY ONLY (TFT Chat Bridge)](#build-2-chat-relay-only-tft-chat-bridge)
- [Build 3: INTEGRATED (Recommended)](#build-3-integrated-recommended)
- [Key Labels (Short + Long)](#key-labels-short--long)
- [UI Mode: Legacy GET vs UI POST](#ui-mode-legacy-get-vs-ui-post)
- [Plugin Icon](#plugin-icon)
- [Troubleshooting](#troubleshooting)

---

## Hardware Overview

### Current test configuration
- **Pico** scans a **6×6** keypad matrix and sends packets over UART to the ESP8266.
- **ESP8266**:
  - receives UART packets on RX (GPIO3)
  - computes short/long press
  - `POST`s key events to the Lumia plugin `/event`
  - hosts TFT endpoints:
    - `GET /msg?t=...`
    - `GET /status?t=...`
    - `GET /clear`
    - `POST /ui` (optional UI JSON endpoint)
    - `GET /health` (device health JSON)

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
Common wiring for typical ESP8266 dev boards:

- TFT **VCC**  → **3V3**
- TFT **GND**  → **GND**
- TFT **SCK**  → **D5 (GPIO14)**
- TFT **MOSI** → **D7 (GPIO13)**
- TFT **CS**   → **D2 (GPIO4)**
- TFT **DC**   → **D1 (GPIO5)**
- TFT **RST**  → **D0 (GPIO16)**

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
3. Arduino IDE → select your ESP8266 board  
4. Open and upload the ESP sketch you want (see builds below)  
5. Reconnect Pico GP0 → ESP RX after upload

---

## Plugin Build + Install (CLI)

Open a terminal in the plugin folder (the folder containing `package.json`).

### Build (Windows)
```bash
npm install
npx lumia-plugin validate .
npx lumia-plugin build . --out .\OUTPUT_NAME.lumiaplugin
```

### Install into Lumia
- Lumia → Configuration → Plugins → Installed → Install Manually  
- Select the `.lumiaplugin` you built

---

# Build 1: 6×6 ONLY (Matrix Relay)

## What it does
- Pico scans keys → ESP sends events to a Lumia plugin
- Lumia plugin triggers two alerts:
  - **6x6 short**
  - **6x6 long**
  Each has **36 variations** via `dynamic.value`

## Firmware used
- Pico: `6x6_test_pico.ino`
- ESP: any “event poster” build that POSTs `{"event":0..71}` to the PC plugin

## Plugin (Lumia)
- Install: `matrix_relay_6x6_ver.lumiaplugin`
- Settings:
  - Enable Listener = ON
  - Listen Port = 8787
  - Shared Secret = (blank)

## ESP config (edit + flash)
```cpp
const char* PLUGIN_HOST = "192.168.1.87"; // your PC IP
constexpr uint16_t PLUGIN_PORT = 8787;    // same as Listen Port
const char* PLUGIN_SECRET = "";           // same as Shared Secret (or blank)
```

---

# Build 2: CHAT RELAY ONLY (TFT Chat Bridge)

## What it does
- Lumia plugin sends display text to the ESP TFT
- Uses:
  - `GET /msg?t=...`
  - `GET /clear`

## Firmware used
- ESP: `espChatBridge.ino` (hardcoded Wi-Fi)

## ESP Wi-Fi (edit + flash)
```cpp
const char* WIFI_SSID = "YourWiFiName";
const char* WIFI_PASS = "YourWiFiPassword";
```

## Plugin (Lumia)
- Install: `ESP8266_TFT_Chat_Bridge.lumiaplugin`
- Settings:
  - ESP Base URL = `http://<ESP_IP>`
  - Message Path = `/msg`
  - Clear Path = `/clear`

## Quick test (browser)
- `http://<ESP_IP>/msg?t=Hello%20world`
- `http://<ESP_IP>/clear`

---

# Build 3: INTEGRATED (Recommended)

## What it does
One ESP firmware + one integrated Lumia plugin:
- Device → PC: `POST /event` (key presses)
- Plugin → device display actions:
  - Display Message → `/msg` (or `/ui` in UI POST mode)
  - Status Message → `/status` (or `/ui`)
  - Clear Screen → `/clear` (or `/ui`)
- “Pro” integrated plugins add:
  - device connected/offline state
  - device status variables (device id, last seen, RSSI, etc.)
  - optional debug toasts

## Recommended pairing
- **Firmware v3**: `lumi_con_esp_integrated_0_0_3.ino`
- **Integrated plugin v5.1**: `lumi_con_bridge_integrated_v5_1.lumiaplugin`

## ESP first-time Wi-Fi (WiFiManager)
1) Power ESP → Wi-Fi AP appears: `Lumi-Con-Setup`  
2) Connect to `Lumi-Con-Setup`  
3) Open: `http://192.168.4.1`  
4) Select home Wi-Fi + password → Save  
5) ESP reboots → TFT shows IP

## Mode selection (firmware v3)
After Wi-Fi connects and IP is shown, the ESP waits for a mode choice using the **matrix keys** (via Pico UART):

- Press **Key 1** → **LEGACY** mode (for older plugins; HTTP 2xx counts as success)
- Press **Key 2** → **CONFIRMED** mode (requires `{ ok:true, seq:<same> }` ACK; TFT shows `ACK OK` / `ACK FAIL`)

## ESP firmware config (edit + flash)
```cpp
const char* PLUGIN_HOST = "192.168.1.87"; // your PC IP
constexpr uint16_t PLUGIN_PORT = 8787;    // same as plugin Listen Port
const char* PLUGIN_SECRET = "";           // same as plugin Shared Secret (or blank)
```

## Integrated plugin (Lumia) minimal settings
- Enable Listener = ON
- Listen Port = 8787
- Shared Secret = (blank)

Optional (for TFT actions):
- ESP Base URL = `http://<ESP_IP>`

Optional (v5.1):
- Enable Debug Toasts = ON

---

## Key Labels (Short + Long)

Integrated plugins that support key maps provide:
- **Key labels (Short 0–35)**: 36 lines, one label per key
- **Key labels (Long 0–35)**: 36 lines, one label per key

The plugin exposes `key_label` (and uses the correct list automatically based on short/long press).

---

## UI Mode: Legacy GET vs UI POST

This setting affects **TFT display only** (not key presses).

### Legacy GET (simple)
Plugin sends:
- `GET http://<ESP_IP>/msg?t=...`
- `GET http://<ESP_IP>/status?t=...`
- `GET http://<ESP_IP>/clear`

### UI POST (JSON)
Plugin sends:
- `POST http://<ESP_IP>/ui`
  - `{"channel":"chat","text":"..."}`
  - `{"channel":"status","text":"..."}`
  - `{"channel":"clear"}`

### Quick UI POST test (Windows PowerShell)
```powershell
curl -X POST "http://<ESP_IP>/ui" -H "Content-Type: application/json" -d "{\"channel\":\"status\",\"text\":\"OK: Connected\"}"
curl -X POST "http://<ESP_IP>/ui" -H "Content-Type: application/json" -d "{\"channel\":\"chat\",\"text\":\"Hello\"}"
curl -X POST "http://<ESP_IP>/ui" -H "Content-Type: application/json" -d "{\"channel\":\"clear\"}"
```

---

## Plugin Icon

1) Put your icon file in the plugin folder (same folder as `manifest.json`)  
   Recommended name: `icon.png`

2) Edit `manifest.json` (top-level, near id/name/version) and add:
```json
"icon": "./icon.png",
```

3) Rebuild:
```bash
npm install
npx lumia-plugin validate .
npx lumia-plugin build . --out .\YOUR_OUTPUT_NAME.lumiaplugin
```

Notes:
- PNG square icons work best (256×256 recommended)
- If validate fails, double-check the icon path matches the filename exactly

---

## Troubleshooting

### ESP upload fails
- Disconnect **Pico GP0 (TX) → ESP RX (GPIO3)** during ESP flashing.

### TFT is white / blank
Try these in order:
1) Change ST7735 tab in code:
   - `INITR_GREENTAB` → `INITR_REDTAB` or `INITR_BLACKTAB`
2) RST on D0 can be flaky:
   - wire TFT RST → ESP RST
   - set RST pin to `-1` in constructor (`Adafruit_ST7735(..., -1)`)

### Lumia alerts don’t fire
- Confirm ESP `PLUGIN_HOST` is your **PC IP**
- Confirm ESP `PLUGIN_PORT` matches plugin **Listen Port**
- If using Shared Secret, both ends must match exactly
- Make sure PC and ESP are on the same LAN

### TFT message actions don’t work
- Confirm plugin setting **ESP Base URL** is correct:
  - must be `http://<ESP_IP>` (no trailing slash)
- Confirm ESP endpoints work in browser:
  - `/msg?t=Hello`
  - `/status?t=OK`
  - `/clear`

### Device health endpoints
- ESP health: `http://<ESP_IP>/health`
- Plugin health (integrated plugin): `http://<PC_IP>:8787/health`
