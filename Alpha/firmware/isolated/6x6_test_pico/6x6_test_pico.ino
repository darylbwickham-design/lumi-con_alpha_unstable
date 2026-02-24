// RP2040 (Raspberry Pi Pico) 6x6 keyboard matrix scanner + UART0 TX to ESP8266
// Matrix Rows (inputs pullup): GP12, GP1..GP5   (GP0 freed for UART TX)
// Matrix Cols (outputs): GP6..GP11
// UART TX: GP0 -> ESP8266 RX (GPIO3 / RX pin)
// USB Serial (debug): unchanged

constexpr uint8_t ROWS = 6;
constexpr uint8_t COLS = 6;

// MOVE ONLY the row wire that was on GP0 to GP12:
const uint8_t rowPins[ROWS] = {12, 1, 2, 3, 4, 5};       // Row0=GP12, Row1..5=GP1..GP5
const uint8_t colPins[COLS] = {6, 7, 8, 9, 10, 11};      // GP6..GP11

constexpr uint32_t DEBOUNCE_MS = 15;

// UART0 TX on GP0 (Serial1 on Earle Philhower RP2040 core)
constexpr uint8_t UART_TX_PIN = 0;     // GP0
constexpr uint8_t UART_RX_PIN = 1;     // GP1 (unused, but set anyway)
constexpr uint32_t UART_BAUD  = 115200;

bool stableState[ROWS][COLS] = {};
bool lastRawState[ROWS][COLS] = {};
uint32_t lastChangeMs[ROWS][COLS] = {};

static inline uint8_t keyNumber(uint8_t r, uint8_t c) {
  // physical x (left->right) = scanned row r
  // physical y (top->bottom) = scanned col c
  uint8_t physCol = r;
  uint8_t physRow = c;
  return (physRow * COLS) + physCol; // 0..35
}

static inline void sendEvent(uint8_t type, uint8_t key) {
  // 4-byte packet: A5, type(1 press / 0 release), key(0..35), xor
  uint8_t b0 = 0xA5;
  uint8_t b1 = type;
  uint8_t b2 = key;
  uint8_t b3 = b0 ^ b1 ^ b2;

  Serial1.write(b0);
  Serial1.write(b1);
  Serial1.write(b2);
  Serial1.write(b3);
}

void setup() {
  Serial.begin(115200);
  delay(600);

  Serial.println("Pico 6x6 Matrix Scanner starting...");
  Serial.println("Rows: GP12, GP1..GP5 (inputs w/ pullups), Cols: GP6..GP11 (scanned low)");
  Serial.println("UART0 TX: GP0 -> ESP RX (GPIO3). Packet: A5 type key xor");
  Serial.println("Format: PRESS y,x key  |  RELEASE y,x key");
  Serial.println("Numbering: top row 0..5 L->R, second row 6..11 L->R, etc.");
  Serial.println();

  // Init UART AFTER USB serial is up
  Serial1.setTX(UART_TX_PIN);
  Serial1.setRX(UART_RX_PIN);
  Serial1.begin(UART_BAUD);

  for (uint8_t r = 0; r < ROWS; r++) {
    pinMode(rowPins[r], INPUT_PULLUP);
  }

  for (uint8_t c = 0; c < COLS; c++) {
    pinMode(colPins[c], OUTPUT);
    digitalWrite(colPins[c], HIGH);
  }

  // Baseline scan (no printing)
  scanMatrix(true);
}

void loop() {
  scanMatrix(false);
}

void scanMatrix(bool initOnly) {
  for (uint8_t c = 0; c < COLS; c++) {
    digitalWrite(colPins[c], LOW);
    delayMicroseconds(40);

    for (uint8_t r = 0; r < ROWS; r++) {
      bool pressedRaw = (digitalRead(rowPins[r]) == LOW);

      if (initOnly) {
        stableState[r][c] = pressedRaw;
        lastRawState[r][c] = pressedRaw;
        lastChangeMs[r][c] = millis();
        continue;
      }

      if (pressedRaw != lastRawState[r][c]) {
        lastRawState[r][c] = pressedRaw;
        lastChangeMs[r][c] = millis();
      }

      uint32_t now = millis();
      if ((now - lastChangeMs[r][c]) >= DEBOUNCE_MS) {
        if (stableState[r][c] != pressedRaw) {
          stableState[r][c] = pressedRaw;

          uint8_t physCol = r; // x
          uint8_t physRow = c; // y
          uint8_t k = keyNumber(r, c);
          uint8_t type = pressedRaw ? 1 : 0;

          if (pressedRaw) Serial.print("PRESS   ");
          else            Serial.print("RELEASE ");

          Serial.print("y=");
          Serial.print(physRow);
          Serial.print(" x=");
          Serial.print(physCol);
          Serial.print("  key=");
          Serial.println(k);

          // transmit to ESP
          sendEvent(type, k);
        }
      }
    }

    digitalWrite(colPins[c], HIGH);
  }
}