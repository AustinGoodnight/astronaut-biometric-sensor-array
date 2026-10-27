// ESP32-C6 (Seeed XIAO) backend. Compiled only by the Arduino build.
#ifdef ARDUINO

#include <Arduino.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <stdarg.h>

#include "battery_test.h"
#include "hal.h"

#ifdef SWAP_I2C // -DSWAP_I2C: test for SCL/SDA crossed on the board
#define SDA_PIN    23 // D5
#define SCL_PIN    22 // D4
#else
#define SDA_PIN    22 // D4, same I2C pins as temp-testing/hr-testing
#define SCL_PIN    23 // D5
#endif
#define SELECT_PIN 16 // D6
#define STATUS_PIN 17 // D7

// MAX17048 is fine at 400 kHz, but the board has no I2C pull-ups of its own
// and the bus runs through the TS5A23157, so stay at standard mode.
#define I2C_FREQ_HZ 100000

bool hal_init() {
  // Idle I2C lines must read high. A line stuck low (unpowered switch/gauge
  // clamping it, a short, or a pull-down) makes every transfer fail.
  pinMode(SDA_PIN, INPUT_PULLUP);
  pinMode(SCL_PIN, INPUT_PULLUP);
  delay(5);
  hal_printf("# Idle bus lines: SDA=%s SCL=%s (both should be HIGH)\n",
             digitalRead(SDA_PIN) ? "HIGH" : "LOW",
             digitalRead(SCL_PIN) ? "HIGH" : "LOW");

  // The board has no I2C pull-ups, so the bus runs on the internal ~45k ones
  // that Wire.begin() turns on. Enabled explicitly so it doesn't depend on
  // the core's default. Weak, but OK at 100 kHz with short wires; add 4.7k to
  // 3V3 on SCL/SDA if the bus is flaky.
  if (!Wire.begin(SDA_PIN, SCL_PIN, I2C_FREQ_HZ)) return false;
  gpio_pullup_en((gpio_num_t)SDA_PIN);
  gpio_pullup_en((gpio_num_t)SCL_PIN);
  delay(5);
  hal_printf("# After Wire.begin: SDA=%s SCL=%s\n",
             gpio_get_level((gpio_num_t)SDA_PIN) ? "HIGH" : "LOW",
             gpio_get_level((gpio_num_t)SCL_PIN) ? "HIGH" : "LOW");
  pinMode(SELECT_PIN, OUTPUT);
  digitalWrite(SELECT_PIN, LOW);
  // No internal pull: STAT only sinks 10uA against the board's 470k, and
  // any internal pull-up would overpower it.
  pinMode(STATUS_PIN, INPUT);
  return true;
}

bool hal_i2c_probe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool hal_i2c_write(uint8_t addr, const uint8_t *buf, size_t len) {
  Wire.beginTransmission(addr);
  Wire.write(buf, len);
  return Wire.endTransmission() == 0;
}

bool hal_i2c_write_read(uint8_t addr, const uint8_t *wbuf, size_t wlen,
                        uint8_t *rbuf, size_t rlen) {
  Wire.beginTransmission(addr);
  Wire.write(wbuf, wlen);
  if (Wire.endTransmission(false) != 0) return false; // repeated start
  if (Wire.requestFrom(addr, (uint8_t)rlen) != rlen) return false;
  for (size_t i = 0; i < rlen; i++) rbuf[i] = Wire.read();
  return true;
}

void hal_set_select(bool high) { digitalWrite(SELECT_PIN, high ? HIGH : LOW); }

bool hal_read_status() { return digitalRead(STATUS_PIN) == HIGH; }

void hal_delay_ms(uint32_t ms) { delay(ms); }

uint32_t hal_millis() { return millis(); }

void hal_printf(const char *fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.print(buf);
}

#ifdef PIN_TEST
// Build with -DPIN_TEST to drive SDA/SCL/SELECT push-pull HIGH and hold
// them, so the pins can be identified with a meter. Nothing else runs.
void setup() {
  Serial.begin(115200);
  const int pins[] = {SDA_PIN, SCL_PIN, SELECT_PIN};
  for (int p : pins) {
    pinMode(p, OUTPUT);
    digitalWrite(p, HIGH);
  }
}

void loop() {
  hal_printf("# PIN_TEST: GPIO%d (D4/SDA), GPIO%d (D5/SCL), GPIO%d (D6/SELECT) "
             "driven HIGH\n", SDA_PIN, SCL_PIN, SELECT_PIN);
  delay(1000);
}
#elif defined(BITBANG_SCAN)
// Build with -DBITBANG_SCAN for a software I2C scan that bypasses the ESP32
// I2C driver. Open-drain emulated: "1" = input with pull-up, "0" = drive low.
static void lineHigh(int p) { pinMode(p, INPUT_PULLUP); }
static void lineLow(int p) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
static const uint32_t HALF_US = 50; // ~10 kHz

// Release SCL and wait for it to rise; false if something holds it low.
static bool sclRelease() {
  lineHigh(SCL_PIN);
  for (int i = 0; i < 1000; i++) {
    if (digitalRead(SCL_PIN)) return true;
    delayMicroseconds(10);
  }
  return false;
}

// START, address + write bit, read ACK, STOP. Returns 1 = ACK, 0 = NACK,
// -1 = SCL stuck low.
static int probe(uint8_t addr) {
  lineHigh(SDA_PIN);
  if (!sclRelease()) return -1;
  delayMicroseconds(HALF_US);
  lineLow(SDA_PIN); delayMicroseconds(HALF_US); // START
  lineLow(SCL_PIN); delayMicroseconds(HALF_US);
  uint8_t byte = addr << 1;
  for (int b = 7; b >= 0; b--) {
    if (byte & (1 << b)) lineHigh(SDA_PIN); else lineLow(SDA_PIN);
    delayMicroseconds(HALF_US);
    if (!sclRelease()) return -1;
    delayMicroseconds(HALF_US);
    lineLow(SCL_PIN);
  }
  lineHigh(SDA_PIN); delayMicroseconds(HALF_US); // release SDA for ACK
  if (!sclRelease()) return -1;
  delayMicroseconds(HALF_US / 2);
  bool ack = digitalRead(SDA_PIN) == LOW;
  delayMicroseconds(HALF_US / 2);
  lineLow(SCL_PIN); delayMicroseconds(HALF_US);
  lineLow(SDA_PIN); delayMicroseconds(HALF_US); // STOP
  sclRelease(); delayMicroseconds(HALF_US);
  lineHigh(SDA_PIN); delayMicroseconds(HALF_US);
  return ack ? 1 : 0;
}

void setup() {
  Serial.begin(115200);
  pinMode(SELECT_PIN, OUTPUT);
  lineHigh(SDA_PIN);
  lineHigh(SCL_PIN);
}

void loop() {
  for (int side = 0; side < 2; side++) {
    digitalWrite(SELECT_PIN, side ? HIGH : LOW);
    delay(5);
    hal_printf("# bitbang SELECT=%s (%s): idle SDA=%d SCL=%d, ACKs:",
               side ? "HIGH" : "LOW", side ? "U4/BAT1" : "U9/BAT2",
               digitalRead(SDA_PIN), digitalRead(SCL_PIN));
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
      int r = probe(a);
      if (r == -1) { hal_printf(" SCL-STUCK-LOW@0x%02X", a); found++; break; }
      if (r == 1) { hal_printf(" 0x%02X", a); found++; }
    }
    hal_printf("%s\n", found ? "" : " none");
  }
  delay(2000);
}
#else
void setup() {
  Serial.begin(115200);
  delay(2000); // give the serial monitor a moment to connect
  battery_test_setup();
}

void loop() { battery_test_loop(); }
#endif

#endif // ARDUINO
