#include <Wire.h>
#include <string.h>
#include "MAX30105.h"
#include "heartRate.h"

#define SDA_PIN 22  // XIAO ESP32C6 default I2C (D4)
#define SCL_PIN 23  // XIAO ESP32C6 default I2C (D5)

// Red LED drive current (0x00-0xFF, ~0.2mA per step, max ~50mA). The right value depends
// on finger/pressure/ambient light, so it's auto-tuned at runtime (see autoAdjustLed())
// instead of hardcoded: too low gives a weak, noisy AC signal; too high saturates the ADC
// (pegs at 262143) and flattens the pulse just as badly.
byte ledBrightness = 0x1F;

MAX30105 particleSensor;
bool sensorReady = false;

const byte RATE_SIZE = 9; // window size for the median filter (odd, so the median is a real sample)
byte rates[RATE_SIZE];
byte rateSpot = 0;
byte beatsCollected = 0; // how many valid beats have gone into rates[] so far (caps at RATE_SIZE)
long lastBeat = 0; // time of the last detected beat, in ms

int beatAvg;

void setup() {
  Serial.begin(115200);
  delay(2000); // give the serial monitor a moment to connect

  Wire.begin(SDA_PIN, SCL_PIN);
}

void tryInitSensor() {
  Serial.println("Probing for MAX30102...");

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30102 not found. Check wiring. Retrying in 1s...");
    return;
  }

  // defaults: 4-sample avg, 400Hz, 411us pulse width, 4096 ADC range.
  particleSensor.setup(ledBrightness);
  particleSensor.setPulseAmplitudeGreen(0); // this board has no green LED
  particleSensor.setPulseAmplitudeRed(0); // unused - only the IR channel is read below

  Serial.println("MAX30102 init succeeded. Place a finger on the sensor.");
  sensorReady = true;
}

// Keeps the raw IR reading in a healthy mid-range: too low is noisy, too high saturates
// the ADC (262143 ceiling) and flattens the pulse. Both look like "checkForBeat never
// converges" from the outside, so this runs continuously rather than being a one-shot
// calibration, since finger pressure and ambient light drift during use.
void autoAdjustLed(long irValue) {
  static unsigned long lastAdjust = 0;
  if (millis() - lastAdjust < 200) return;
  lastAdjust = millis();

  if (irValue > 240000 && ledBrightness > 0x02) {
    ledBrightness -= 4;
    particleSensor.setPulseAmplitudeIR(ledBrightness);
  } else if (irValue > 50000 && irValue < 80000 && ledBrightness < 0x7F) {
    ledBrightness += 4;
    particleSensor.setPulseAmplitudeIR(ledBrightness);
  }
}

// checkForBeat() has no built-in plausibility check, so a noise blip or a secondary bump
// in the pulse waveform (dicrotic notch) can misfire early (too-high instant BPM) or miss
// a beat entirely (too-low instant BPM, since the interval spans 2+ real beats). Both kinds
// of error show up in either direction, so a median over a window is what actually rejects
// them - unlike comparing each new reading against a running average, which lets a string
// of similar-looking bad readings drag the "reference" itself off the true rate and then
// reject good ones for disagreeing with the now-wrong reference.
void handleBeat(float instBpm) {
  Serial.print("beat: ");
  Serial.print(instBpm, 1);
  Serial.print(" bpm -> ");

  if (instBpm <= 42 || instBpm >= 220) {
    Serial.println("REJECTED (outside human range)");
    return;
  }

  rates[rateSpot++] = (byte)instBpm;
  rateSpot %= RATE_SIZE;
  if (beatsCollected < RATE_SIZE) beatsCollected++;

  byte sorted[RATE_SIZE];
  memcpy(sorted, rates, beatsCollected);
  for (byte i = 1; i < beatsCollected; i++) {
    byte key = sorted[i];
    int j = i - 1;
    while (j >= 0 && sorted[j] > key) {
      sorted[j + 1] = sorted[j];
      j--;
    }
    sorted[j + 1] = key;
  }
  beatAvg = sorted[beatsCollected / 2]; // median - robust to outliers on either side

  Serial.print("KEPT (median now ");
  Serial.print(beatAvg);
  Serial.println(")");
}

void loop() {
  if (!sensorReady) {
    tryInitSensor();
    delay(1000);
    return;
  }

  long irValue = particleSensor.getIR();
  autoAdjustLed(irValue);

  if (checkForBeat(irValue)) {
    long delta = millis() - lastBeat;
    lastBeat = millis();
    handleBeat(60 / (delta / 1000.0));
  }

  // Status line once a second instead of flooding every ~10ms sample - the beat events
  // above are the interesting part; this is just a heartbeat that things are still running.
  static unsigned long lastStatus = 0;
  if (millis() - lastStatus >= 1000) {
    lastStatus = millis();
    if (irValue < 50000) {
      Serial.println("No finger detected");
    } else {
      Serial.print("status: ir=");
      Serial.print(irValue);
      Serial.print(" avgBpm=");
      Serial.print(beatsCollected > 0 ? String(beatAvg) : String("--"));
      Serial.print(" led=");
      Serial.println(ledBrightness);
    }
  }
}
