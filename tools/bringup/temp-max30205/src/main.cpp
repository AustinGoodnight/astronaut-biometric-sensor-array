#include <Arduino.h>
#include <Wire.h>

#define SDA_PIN 22  // XIAO ESP32C6 default I2C (D4)
#define SCL_PIN 23  // XIAO ESP32C6 default I2C (D5)

// MAX30205 register map (datasheet table 1). There's no ID register, so the
// power-on defaults of THYST/TOS are used below as an identity check instead.
#define REG_TEMP   0x00
#define REG_CONFIG 0x01
#define REG_THYST  0x02
#define REG_TOS    0x03

#define THYST_DEFAULT 0x4B00 // 75 C
#define TOS_DEFAULT   0x5000 // 80 C

// Address is set by the A0/A1/A2 straps (0x48-0x4F for GND/VDD combinations,
// more with SDA/SCL straps). Scanned at startup rather than hardcoded so a
// different breakout doesn't need a code change.
const byte ADDR_FIRST = 0x40;
const byte ADDR_LAST = 0x5F;

const unsigned long SAMPLE_INTERVAL_MS = 1000;

byte sensorAddr = 0;
bool sensorReady = false;

bool writeReg8(byte reg, byte value) {
  Wire.beginTransmission(sensorAddr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readReg16(byte reg, uint16_t &out) {
  Wire.beginTransmission(sensorAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false; // repeated start
  if (Wire.requestFrom(sensorAddr, (byte)2) != 2) return false;
  out = ((uint16_t)Wire.read() << 8) | Wire.read();
  return true;
}

void scanBus() {
  Serial.println("I2C scan:");
  byte found = 0;
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  device at 0x%02X\n", addr);
      found++;
    }
  }
  if (found == 0) Serial.println("  no devices found");
}

void tryInitSensor() {
  scanBus();

  for (byte addr = ADDR_FIRST; addr <= ADDR_LAST; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() != 0) continue;

    sensorAddr = addr;
    uint16_t thyst, tos;
    if (!readReg16(REG_THYST, thyst) || !readReg16(REG_TOS, tos)) continue;

    Serial.printf("0x%02X: THYST=0x%04X TOS=0x%04X", addr, thyst, tos);
    if (thyst == THYST_DEFAULT && tos == TOS_DEFAULT) {
      Serial.println(" -> matches MAX30205 defaults");
    } else {
      // Not fatal: the registers are writable and retain values across a
      // soft reset, so a previously-configured chip won't match.
      Serial.println(" -> doesn't match MAX30205 power-on defaults, trying anyway");
    }

    // Continuous conversion, normal data format (0-50 C range isn't clipped),
    // comparator mode, no timeout disable. Everything else at defaults.
    if (!writeReg8(REG_CONFIG, 0x00)) {
      Serial.println("Config write failed. Retrying in 1s...");
      return;
    }

    Serial.printf("MAX30205 init succeeded at 0x%02X.\n", sensorAddr);
    Serial.println("temp_c,temp_f"); // CSV header for the lines that follow
    sensorReady = true;
    return;
  }

  Serial.println("MAX30205 not found. Check wiring. Retrying in 1s...");
}

void setup() {
  Serial.begin(115200);
  delay(2000); // give the serial monitor a moment to connect

  Wire.begin(SDA_PIN, SCL_PIN);
}

void loop() {
  if (!sensorReady) {
    tryInitSensor();
    delay(1000);
    return;
  }

  static unsigned long lastSample = 0;
  if (millis() - lastSample < SAMPLE_INTERVAL_MS) return;
  lastSample = millis();

  uint16_t raw;
  if (!readReg16(REG_TEMP, raw)) {
    Serial.println("Read failed - sensor disconnected? Re-probing.");
    sensorReady = false;
    return;
  }

  // 16-bit two's complement, LSB = 1/256 C (0.00390625 C).
  float tempC = (int16_t)raw / 256.0f;
  float tempF = tempC * 9.0f / 5.0f + 32.0f;
  Serial.printf("%.3f,%.3f\n", tempC, tempF);
}
