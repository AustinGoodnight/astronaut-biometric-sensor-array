#include <Wire.h>
#include <string.h>

#include "MAX30105.h"
#include "spo2_algorithm.h"

MAX30105 particleSensor;
bool sensorReady = false;
bool primed = false;

// 100 samples at 25 samples/sec (sampleAverage=4, sampleRate=100) = 4 seconds
// of history, matching Maxim's reference algorithm window.
const byte BUFFER_LENGTH = 100;
uint32_t irBuffer[BUFFER_LENGTH];
uint32_t redBuffer[BUFFER_LENGTH];

int32_t spo2;
int8_t validSPO2;
int32_t heartRate;
int8_t validHeartRate;

void setup() {
  Serial.begin(115200);
  delay(2000);

  // XIAO nRF52840 default I2C pins are D4 (SDA) and D5 (SCL).
  Wire.begin();
}

void tryInitSensor() {
  Serial.println("Probing for MAX30102...");

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30102 not found. Check wiring. Retrying in 1s...");
    return;
  }

  // Red + IR mode; both channels are required by the SpO2 algorithm.
  particleSensor.setup(0x3C, 4, 2, 100, 411, 4096);

  Serial.println("MAX30102 init succeeded (Maxim SpO2/HR algorithm). Place a finger on the sensor.");
  sensorReady = true;
}

void fillSamples(byte start, byte count) {
  for (byte i = start; i < start + count; i++) {
    while (!particleSensor.available()) {
      particleSensor.check();
    }
    redBuffer[i] = particleSensor.getRed();
    irBuffer[i] = particleSensor.getIR();
    particleSensor.nextSample();
  }
}

void updateMeasurements() {
  maxim_heart_rate_and_oxygen_saturation(irBuffer, BUFFER_LENGTH, redBuffer, &spo2,
                                         &validSPO2, &heartRate, &validHeartRate);

  if (irBuffer[BUFFER_LENGTH - 1] < 50000) {
    Serial.println("No finger detected");
    return;
  }

  Serial.print("HR=");
  Serial.print(heartRate);
  Serial.print(" (valid=");
  Serial.print(validHeartRate);
  Serial.print(") SPO2=");
  Serial.print(spo2);
  Serial.print(" (valid=");
  Serial.print(validSPO2);
  Serial.println(")");
}

void loop() {
  if (!sensorReady) {
    tryInitSensor();
    delay(1000);
    return;
  }

  if (!primed) {
    // Fill the complete four-second window before producing the first estimate.
    fillSamples(0, BUFFER_LENGTH);
    primed = true;
  } else {
    // Slide by 25 samples (one second) as in Maxim's reference usage.
    memmove(redBuffer, redBuffer + 25, 75 * sizeof(uint32_t));
    memmove(irBuffer, irBuffer + 25, 75 * sizeof(uint32_t));
    fillSamples(75, 25);
  }

  updateMeasurements();
}
