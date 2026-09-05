// Does the Ra-01SH (SX1262) on this Rev 2 actually work?
//
// Written after a reflow to clear a solder bridge between GND and the module's
// BUSY pad. BUSY is the worst pin on an SX126x to have shorted: the driver
// waits on it before and after every command, so stuck LOW reads as "always
// ready" and the host talks over the chip, while stuck HIGH makes every
// operation time out. Both look like a dead radio from the firmware.
//
// The decisive observation is simple: BUSY must be seen HIGH at least once.
// A pin shorted to ground physically cannot go high, so a single confirmed
// high reading clears the bridge -- no continuity meter needed. Everything
// after that checks the remaining joints: SPI in both directions, and whether
// the part answers as an SX1262.

#include <Arduino.h>
#include <SPI.h>

// IMPR RAD-01 Rev 2, from Boards.h (BOARD_RAD01_REV2).
constexpr int PIN_CS = 13;
constexpr int PIN_RESET = 14;
constexpr int PIN_SCLK = 10;
constexpr int PIN_MOSI = 11;
constexpr int PIN_MISO = 12;
constexpr int PIN_BUSY = 5;
constexpr int PIN_DIO1 = 6;

// SX126x opcodes
constexpr uint8_t OP_GET_STATUS = 0xC0;
constexpr uint8_t OP_WRITE_REG = 0x0D;
constexpr uint8_t OP_READ_REG = 0x1D;
constexpr uint8_t OP_SET_STANDBY = 0x80;
constexpr uint8_t OP_GET_DEVICE_ERRORS = 0x17;
// LoRa sync-word registers: writable, and a safe scratch for a round trip.
constexpr uint16_t REG_LORA_SYNCWORD = 0x0740;

SPIClass radioSPI(FSPI);
bool busy_seen_high = false;
uint32_t busy_high_us_after_reset = 0;

static bool waitBusyLow(uint32_t timeout_us) {
  const uint32_t start = micros();
  while (digitalRead(PIN_BUSY) == HIGH) {
    busy_seen_high = true;
    if (micros() - start > timeout_us) return false;
  }
  return true;
}

static void beginTransfer() {
  radioSPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CS, LOW);
}

static void endTransfer() {
  digitalWrite(PIN_CS, HIGH);
  radioSPI.endTransaction();
}

static uint8_t getStatus() {
  waitBusyLow(10000);
  beginTransfer();
  radioSPI.transfer(OP_GET_STATUS);
  const uint8_t status = radioSPI.transfer(0x00);
  endTransfer();
  return status;
}

static void writeRegister(uint16_t addr, uint8_t value) {
  waitBusyLow(10000);
  beginTransfer();
  radioSPI.transfer(OP_WRITE_REG);
  radioSPI.transfer((uint8_t)(addr >> 8));
  radioSPI.transfer((uint8_t)(addr & 0xFF));
  radioSPI.transfer(value);
  endTransfer();
}

static uint8_t readRegister(uint16_t addr) {
  waitBusyLow(10000);
  beginTransfer();
  radioSPI.transfer(OP_READ_REG);
  radioSPI.transfer((uint8_t)(addr >> 8));
  radioSPI.transfer((uint8_t)(addr & 0xFF));
  radioSPI.transfer(0x00);            // NOP while the part fetches
  const uint8_t value = radioSPI.transfer(0x00);
  endTransfer();
  return value;
}

static void setStandby(uint8_t mode) {
  waitBusyLow(10000);
  beginTransfer();
  radioSPI.transfer(OP_SET_STANDBY);
  radioSPI.transfer(mode);
  endTransfer();
}

static uint16_t getDeviceErrors() {
  waitBusyLow(10000);
  beginTransfer();
  radioSPI.transfer(OP_GET_DEVICE_ERRORS);
  radioSPI.transfer(0x00);
  const uint8_t hi = radioSPI.transfer(0x00);
  const uint8_t lo = radioSPI.transfer(0x00);
  endTransfer();
  return ((uint16_t)hi << 8) | lo;
}

static void resetRadio() {
  digitalWrite(PIN_RESET, LOW);
  delay(2);
  digitalWrite(PIN_RESET, HIGH);
  // BUSY rises while the part boots. Time how long it stays up: that is the
  // single most direct evidence the pin is connected and free to move.
  const uint32_t start = micros();
  while (digitalRead(PIN_BUSY) == LOW && (micros() - start) < 50000) { }
  if (digitalRead(PIN_BUSY) == HIGH) {
    busy_seen_high = true;
    const uint32_t high_start = micros();
    while (digitalRead(PIN_BUSY) == HIGH && (micros() - high_start) < 200000) { }
    busy_high_us_after_reset = micros() - high_start;
  }
}

static void runProbe() {
  busy_seen_high = false;
  busy_high_us_after_reset = 0;
  Serial.println();
  Serial.println("=== Rev 2 LoRa probe (Ra-01SH / SX1262) ===");

  pinMode(PIN_BUSY, INPUT);
  pinMode(PIN_DIO1, INPUT);
  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_RESET, OUTPUT);
  digitalWrite(PIN_RESET, HIGH);

  // A pin held to ground cannot be pulled up. If this reads LOW with the
  // internal pull-up on and the radio out of reset and idle, that is the
  // signature of the bridge still being present.
  pinMode(PIN_BUSY, INPUT_PULLUP);
  delay(5);
  const int busy_pulled_up = digitalRead(PIN_BUSY);
  pinMode(PIN_BUSY, INPUT_PULLDOWN);
  delay(5);
  const int busy_pulled_down = digitalRead(PIN_BUSY);
  pinMode(PIN_BUSY, INPUT);
  Serial.printf("BUSY with pull-up: %s, with pull-down: %s\n",
                busy_pulled_up ? "HIGH" : "LOW",
                busy_pulled_down ? "HIGH" : "LOW");

  radioSPI.begin(PIN_SCLK, PIN_MISO, PIN_MOSI, PIN_CS);

  resetRadio();
  Serial.printf("BUSY high for %lu us after reset\n",
                (unsigned long)busy_high_us_after_reset);

  setStandby(0x00);   // STDBY_RC
  const uint8_t status = getStatus();
  Serial.printf("GetStatus: 0x%02X (chipmode=%u cmdstatus=%u)\n",
                status, (unsigned)((status >> 4) & 0x07),
                (unsigned)((status >> 1) & 0x07));

  // SPI round trip. If MOSI, MISO, SCLK and CS are all good and the part is
  // alive, what goes out comes back.
  const uint8_t original = readRegister(REG_LORA_SYNCWORD);
  writeRegister(REG_LORA_SYNCWORD, 0xA5);
  const uint8_t readback = readRegister(REG_LORA_SYNCWORD);
  writeRegister(REG_LORA_SYNCWORD, original);
  const uint8_t restored = readRegister(REG_LORA_SYNCWORD);
  Serial.printf("Register 0x%04X: original 0x%02X, wrote 0xA5 read 0x%02X, restored 0x%02X\n",
                REG_LORA_SYNCWORD, original, readback, restored);

  Serial.printf("GetDeviceErrors: 0x%04X\n", getDeviceErrors());

  Serial.println();
  Serial.println("--- verdict ---");
  Serial.printf("BUSY ever observed HIGH : %s\n", busy_seen_high ? "YES" : "NO");
  Serial.printf("SPI round trip          : %s\n", (readback == 0xA5) ? "PASS" : "FAIL");
  if (!busy_seen_high) {
    Serial.println("BUSY never went high. Either it is still tied to ground, or the");
    Serial.println("part is not running at all -- check the SPI result to tell which.");
  }
  if (readback != 0xA5) {
    Serial.println("SPI did not read back what was written. Suspect CS, SCLK, MOSI,");
    Serial.println("MISO, or the module's supply -- not BUSY.");
  }
  if (busy_seen_high && readback == 0xA5) {
    Serial.println("Radio answers and BUSY moves. The bridge is gone and every");
    Serial.println("castellation in the SPI path is making contact.");
  }
}

void setup() {
  Serial.begin(115200);
  const uint32_t wait_start = millis();
  while (!Serial && millis() - wait_start < 4000) { delay(10); }
  delay(300);
  runProbe();
}

void loop() {
  // Re-run the whole check periodically. The verdict is the point, and a
  // marginal joint shows up as an intermittent result rather than a single
  // lucky reading -- which a one-shot check in setup() would hide.
  static uint32_t last = 0;
  if (millis() - last >= 15000) {
    last = millis();
    runProbe();
  }
}
