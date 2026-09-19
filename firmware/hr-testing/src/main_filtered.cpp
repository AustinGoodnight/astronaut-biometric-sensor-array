#include <Wire.h>
#include <string.h>
#include "MAX30105.h"
#include <NimBLEDevice.h>

#define SDA_PIN 22  // XIAO ESP32C6 default I2C (D4)
#define SCL_PIN 23  // XIAO ESP32C6 default I2C (D5)

// checkForBeat() (used in main.cpp) already has its own internal DC-removal + FIR
// low-pass filtering, so chaining another bandpass filter in front of it would be
// redundant and would break its internal amplitude thresholds (tuned for its own
// raw-ish input scale). This file implements its own complete pipeline instead:
// bandpass filter -> refractory-gated peak detection -> Red/IR cross-check -> median.
byte ledBrightness = 0x1F;

MAX30105 particleSensor;
bool sensorReady = false;

// Nordic UART Service - the de facto standard layout for "BLE serial", so any generic
// BLE terminal app (nRF Connect, LightBlue, etc.) can read this without custom software.
// TX = board-to-host notifications (what we use); RX exists in the spec but is unused here.
#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_CHAR_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// Not part of the NUS spec - our own characteristic on the same service for batched raw
// sample streaming (see RawSample/RAW_BATCH_SIZE below), separate from the text log so a
// binary parser on the host doesn't have to share a channel with human-readable lines.
#define RAW_CHAR_UUID "6E400004-B5A3-F393-E0A9-E50E24DCCA9E"

NimBLECharacteristic *txCharacteristic;
NimBLECharacteristic *rawCharacteristic;
bool bleConnected = false;

// NimBLE's onConnect/onDisconnect take a NimBLEConnInfo& (and onDisconnect a reason
// code) - a different signature than the bundled BLEDevice API's callbacks.
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server, NimBLEConnInfo &connInfo) override {
    bleConnected = true;
    // Without this, the central (observed on macOS) negotiates a connection interval up
    // near the spec max (~4s), so only one notification gets through per interval and
    // everything else is silently dropped - fine for occasional text lines, useless for
    // a ~100Hz raw sample stream. Request 7.5-15ms/no slave latency instead; args are
    // (connHandle, minInterval, maxInterval [x1.25ms units], latency, timeout [x10ms]).
    server->updateConnParams(connInfo.getConnHandle(), 6, 12, 0, 200);
  }
  void onDisconnect(NimBLEServer *server, NimBLEConnInfo &connInfo, int reason) override {
    bleConnected = false;
    server->getAdvertising()->start(); // keep advertising so a new central can reconnect
  }
};

void setupBLE() {
  NimBLEDevice::init("HR-Testing");
  // Default ATT MTU (23 bytes) only leaves 20 bytes of notify payload - not enough for a
  // useful raw-sample batch. Ask for more; the host may negotiate down, but macOS grants it.
  NimBLEDevice::setMTU(247);

  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService *service = server->createService(NUS_SERVICE_UUID);
  txCharacteristic = service->createCharacteristic(NUS_TX_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);
  rawCharacteristic = service->createCharacteristic(RAW_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);
  service->start();

  // Unlike the bundled BLEDevice API (where name + 128-bit NUS UUID together silently
  // broke advertising), NimBLE's advertising payload handling fits both fine - confirmed
  // by scanning and seeing both the "HR-Testing" name and the NUS UUID advertised at once.
  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(NUS_SERVICE_UUID);
  advertising->setPreferredParams(6, 12); // advertise the same fast interval hint up front
  advertising->start();
}

// Every log line goes to both USB serial (for a wired monitor) and BLE notify (for the
// wireless logger) so neither transport requires the other to be connected.
void logLine(const String &line) {
  Serial.println(line);
  if (bleConnected) {
    txCharacteristic->setValue(line.c_str());
    txCharacteristic->notify();
  }
}

// Packed, not a struct-of-arrays: sent byte-for-byte over BLE, so layout must be exact
// (no compiler-inserted padding) and match the Python side's struct.unpack format.
#pragma pack(push, 1)
struct RawSample {
  uint32_t ir;
  uint32_t red;
};
#pragma pack(pop)

// At MTU=247 the usable notify payload is ~244 bytes (247 - 3 byte ATT header); a 2-byte
// sequence number + 24 samples * 8 bytes = 194 bytes comfortably fits with room to spare.
const byte RAW_BATCH_SIZE = 24;
RawSample rawBatch[RAW_BATCH_SIZE];
byte rawBatchCount = 0;
uint16_t rawSeq = 0;

// Buffers one sample per loop iteration and flushes a batch once full, rather than
// notifying per-sample - cuts the notify() call rate ~24x, which matters because each
// call still costs a full BLE connection event even with the tightened interval above.
void sendRawSample(uint32_t irRaw, uint32_t redRaw) {
  rawBatch[rawBatchCount].ir = irRaw;
  rawBatch[rawBatchCount].red = redRaw;
  rawBatchCount++;

  if (rawBatchCount < RAW_BATCH_SIZE) return;
  rawBatchCount = 0;

  if (!bleConnected) return;
  uint8_t packet[2 + sizeof(rawBatch)];
  memcpy(packet, &rawSeq, 2);
  memcpy(packet + 2, rawBatch, sizeof(rawBatch));
  rawSeq++;

  rawCharacteristic->setValue(packet, sizeof(packet));
  rawCharacteristic->notify();
}

// Single-pole DC blocker (high-pass) followed by single-pole low-pass, approximating a
// ~0.5-4 Hz bandpass (30-240 bpm) at our ~100 sps effective sample rate (400Hz/4avg).
struct BandpassFilter {
  float dcPrevX = 0, dcPrevY = 0;
  float lpPrevY = 0;

  float process(float x) {
    float dcY = x - dcPrevX + 0.97f * dcPrevY; // ~0.5 Hz high-pass cutoff
    dcPrevX = x;
    dcPrevY = dcY;

    lpPrevY = 0.75f * lpPrevY + 0.25f * dcY; // ~4 Hz low-pass cutoff
    return lpPrevY;
  }
};

BandpassFilter irFilter;
BandpassFilter redFilter;
float irPrevFiltered = 0;
float redPrevFiltered = 0;

// Smoothed Red slope instead of a raw single-sample derivative: Red's absorption swing is
// smaller/noisier than IR's, so a strict instantaneous-sample check would false-reject real
// beats whenever Red happened to dip on the exact sample IR crossed zero. Averaging the slope
// over a few samples (single-pole again, matching the filter style above) rides through that
// noise while still requiring a genuine upward trend.
float redSlopeAvg = 0;

const unsigned long REFRACTORY_MS = 300; // caps at 200 bpm; blocks double-triggering on
                                          // the dicrotic notch or filter ripple
unsigned long lastBeatMs = 0;

const byte RATE_SIZE = 9; // median filter window (odd, so the median is a real sample)
byte rates[RATE_SIZE];
byte rateSpot = 0;
byte beatsCollected = 0;
int beatAvg;

// Physiological rate-of-change limit: a new beat may differ from the current median by at
// most MAX_JUMP_BPM, plus JUMP_ALLOWANCE_PER_SEC for every second since the last accepted
// beat. E.g. 80 -> 120 is rejected for several seconds. The reference is the median of
// *accepted* beats only, so rejected readings can't drag it off; and the allowance growing
// with time means a wrong starting median (or a real HR change) can't lock detection out.
const float MAX_JUMP_BPM = 10;
const float JUMP_ALLOWANCE_PER_SEC = 5;
const byte MIN_BEATS_FOR_JUMP_CHECK = 3; // need a stable-ish median before trusting it
unsigned long lastKeptMs = 0;

// A session is started/stopped with a button, instead of trying to infer finger presence
// from the signal. Everything that happens outside a session (sensor on the desk, finger
// being placed/removed) is ignored, so it can't pollute the beat history or median.
// External momentary button on D0 (GPIO0), other leg to GND; uses the internal pull-up, so
// no resistor needed. (The onboard BOOT button on GPIO9 also works if you set this to 9.)
#define BUTTON_PIN 0
const unsigned long BUTTON_DEBOUNCE_MS = 50;
const byte DEFAULT_LED = 0x1F;
bool sessionActive = false;
unsigned long sessionStartMs = 0;

void startSession();
void stopSession();

// Calibration: the first seconds of a session are spent collecting the filtered IR waveform
// instead of trusting individual beats. One bad early beat (e.g. a 40 bpm misfire) would
// otherwise become the median that the jump check then defends, rejecting all the real ones.
// The dominant pulse period found by autocorrelation over the whole window seeds the median.
const unsigned long CAL_SETTLE_MS = 2000;   // skip the bandpass filter's startup transient
const unsigned long CAL_WINDOW_MS = 30000;  // data collected after the settle period
const int CAL_MAX_SAMPLES = 4000;           // ~40s at ~100 sps; float buffer is 16KB
const float CAL_MIN_CORRELATION = 0.3f;     // below this the "pulse" isn't periodic enough to trust
float calBuf[CAL_MAX_SAMPLES];
int calCount = 0;
unsigned long calStartMs = 0;
bool calibrating = false;

void setup() {
  Serial.begin(115200);
  delay(2000); // give the serial monitor a moment to connect

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  setupBLE();
  Wire.begin(SDA_PIN, SCL_PIN);
}

void tryInitSensor() {
  logLine("Probing for MAX30102...");

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    logLine("MAX30102 not found. Check wiring. Retrying in 1s...");
    return;
  }

  // ledMode=2 is Red+IR - this pipeline needs both channels for the corroboration check.
  particleSensor.setup(ledBrightness, 4, 2, 400, 411, 4096);

  logLine("MAX30102 init succeeded (bandpass + refractory + dual-channel). Press the button to start a session.");
  lastBeatMs = millis();
  sensorReady = true;
}

// Same idea as main.cpp: keep the raw IR reading out of both the noise floor and ADC
// saturation (262143), since either one flattens the pulsatile signal the same way.
void autoAdjustLed(long irValue) {
  static unsigned long lastAdjust = 0;
  if (millis() - lastAdjust < 200) return;
  lastAdjust = millis();

  if (irValue > 240000 && ledBrightness > 0x02) {
    ledBrightness -= 4;
    particleSensor.setPulseAmplitudeIR(ledBrightness);
  } else if (sessionActive && irValue > 50000 && irValue < 80000 && ledBrightness < 0x7F) {
    // Only ramp up during a session - otherwise a no-finger reflection in this range
    // gets driven higher and higher.
    ledBrightness += 4;
    particleSensor.setPulseAmplitudeIR(ledBrightness);
  }
}

void handleBeat(float instBpm) {
  String line = "beat: " + String(instBpm, 1) + " bpm -> ";

  if (instBpm <= 42 || instBpm >= 220) {
    logLine(line + "REJECTED (outside human range)");
    return;
  }

  if (beatsCollected >= MIN_BEATS_FOR_JUMP_CHECK) {
    float secSinceKept = (millis() - lastKeptMs) / 1000.0f;
    float allowed = MAX_JUMP_BPM + JUMP_ALLOWANCE_PER_SEC * secSinceKept;
    if (fabsf(instBpm - beatAvg) > allowed) {
      logLine(line + "REJECTED (jump from median " + String(beatAvg) + " exceeds " +
              String(allowed, 0) + " bpm)");
      return;
    }
  }

  lastKeptMs = millis();
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
  beatAvg = sorted[beatsCollected / 2];

  logLine(line + "KEPT (median now " + String(beatAvg) + ")");
}

// Autocorrelation of the (mean-removed) window: a periodic pulse correlates strongly with
// itself shifted by one beat period. Lags are searched only over the human range (42-220
// bpm), and the *first* strong peak is taken rather than the global max, since multiples of
// the true period (half the bpm) correlate almost as well.
void finishCalibration() {
  int n = calCount;
  float fs = n / ((millis() - calStartMs) / 1000.0f); // measure the real rate, don't assume 100

  float mean = 0;
  for (int i = 0; i < n; i++) mean += calBuf[i];
  mean /= n;
  float r0 = 0;
  for (int i = 0; i < n; i++) {
    calBuf[i] -= mean;
    r0 += calBuf[i] * calBuf[i];
  }
  r0 /= n;

  static float ac[320];
  int lagMin = (int)(fs * 60.0f / 220.0f);
  int lagMax = min((int)(fs * 60.0f / 42.0f), 318);
  if (lagMin < 2) lagMin = 2;
  for (int lag = lagMin - 1; lag <= lagMax + 1; lag++) {
    float sum = 0;
    for (int i = 0; i + lag < n; i++) sum += calBuf[i] * calBuf[i + lag];
    ac[lag] = (sum / (n - lag)) / r0; // normalized: 1.0 = perfectly periodic at this lag
  }

  float rmax = 0;
  for (int lag = lagMin; lag <= lagMax; lag++) rmax = max(rmax, ac[lag]);

  int best = -1;
  if (r0 > 0 && rmax >= CAL_MIN_CORRELATION) {
    for (int lag = lagMin; lag <= lagMax; lag++) {
      if (ac[lag] >= ac[lag - 1] && ac[lag] >= ac[lag + 1] && ac[lag] >= 0.85f * rmax) {
        best = lag;
        break;
      }
    }
  }

  if (best < 0) {
    logLine("CALIBRATION FAILED (weak/irregular signal, r=" + String(rmax, 2) +
            " need>=" + String(CAL_MIN_CORRELATION, 2) + ", rms=" + String(sqrtf(r0), 0) +
            ", fs=" + String(fs, 0) + "Hz, n=" + String(n) + ") - retrying; check finger placement");
    calCount = 0;
    calStartMs = 0;
    return;
  }

  int baseline = (int)(fs * 60.0f / best + 0.5f);
  for (byte i = 0; i < MIN_BEATS_FOR_JUMP_CHECK; i++) rates[i] = (byte)baseline;
  rateSpot = MIN_BEATS_FOR_JUMP_CHECK % RATE_SIZE;
  beatsCollected = MIN_BEATS_FOR_JUMP_CHECK;
  beatAvg = baseline;
  lastKeptMs = millis();
  calibrating = false;
  logLine("CALIBRATION DONE: baseline=" + String(baseline) + " bpm (r=" + String(ac[best], 2) +
          ", fs=" + String(fs, 0) + "Hz, n=" + String(n) + ")");
}

void startSession() {
  irFilter = BandpassFilter();
  redFilter = BandpassFilter();
  irPrevFiltered = redPrevFiltered = redSlopeAvg = 0;
  beatsCollected = 0;
  rateSpot = 0;
  beatAvg = 0;
  ledBrightness = DEFAULT_LED;
  particleSensor.setPulseAmplitudeIR(ledBrightness);
  sessionStartMs = lastBeatMs = lastKeptMs = millis();
  calCount = 0;
  calStartMs = 0;
  calibrating = true;
  sessionActive = true;
  logLine("SESSION START - calibrating for ~30s, keep finger still");
}

void stopSession() {
  sessionActive = false;
  calibrating = false;
  String summary = "SESSION STOP (" + String((millis() - sessionStartMs) / 1000) + "s, ";
  summary += beatsCollected > 0 ? "final avgBpm=" + String(beatAvg) : String("no valid beats");
  logLine(summary + ")");
}

// Toggles on a debounced press (falling edge; button is active low with the internal pull-up).
void pollButton() {
  static bool lastReading = HIGH;
  static bool stableState = HIGH;
  static unsigned long lastChangeMs = 0;

  bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastReading) {
    lastChangeMs = millis();
    lastReading = reading;
  }
  if (millis() - lastChangeMs >= BUTTON_DEBOUNCE_MS && reading != stableState) {
    stableState = reading;
    if (stableState == LOW) {
      if (sessionActive) stopSession(); else startSession();
    }
  }
}

void loop() {
  if (!sensorReady) {
    tryInitSensor();
    delay(1000);
    return;
  }

  // getIR()/getRed() each independently block on the library's own check(), which pulls
  // whatever new FIFO entry shows up next - so calling them back-to-back drains two
  // different physical samples (one per call) instead of the single paired Red+IR
  // reading the sensor actually produced together. That desyncs the two channels by one
  // sample every iteration and silently burns through the FIFO at 2x the real rate.
  // check() + available()/getFIFOIR()/getFIFORed()/nextSample() is the library's
  // intended pattern for pulling both channels of the same sample together.
  pollButton();
  particleSensor.check();

  while (particleSensor.available()) {
    long irRaw = particleSensor.getFIFOIR();
    long redRaw = particleSensor.getFIFORed();
    particleSensor.nextSample();

    autoAdjustLed(irRaw);
    sendRawSample(irRaw, redRaw);

    if (!sessionActive) {
      static unsigned long lastIdleStatus = 0;
      if (millis() - lastIdleStatus >= 1000) {
        lastIdleStatus = millis();
        logLine("Idle - press button to start session");
      }
      continue;
    }

    float irFiltered = irFilter.process(irRaw);
    float redFiltered = redFilter.process(redRaw);
    redSlopeAvg = 0.7f * redSlopeAvg + 0.3f * (redFiltered - redPrevFiltered);
    unsigned long nowMs = millis();

    if (calibrating && nowMs - sessionStartMs >= CAL_SETTLE_MS) {
      if (calCount == 0) calStartMs = nowMs;
      if (calCount < CAL_MAX_SAMPLES) calBuf[calCount++] = irFiltered;
      if (nowMs - calStartMs >= CAL_WINDOW_MS || calCount == CAL_MAX_SAMPLES) finishCalibration();
    }

    bool risingEdge = (irPrevFiltered <= 0) && (irFiltered > 0);
    if (risingEdge && (nowMs - lastBeatMs) >= REFRACTORY_MS) {
      // A real pulse shows up as a (wavelength-dependent) absorption change on both LEDs at
      // essentially the same instant; single-channel noise usually won't. Requiring Red to
      // also be trending upward is a cheap way to reject those without full correlation math.
      bool redCorroborates = redSlopeAvg > 0;
      if (calibrating) {
        lastBeatMs = nowMs; // keep the edge timing warm, but don't score beats yet
      } else if (!redCorroborates) {
        logLine("candidate beat REJECTED (no Red-channel corroboration)");
      } else {
        float instBpm = 60000.0 / (nowMs - lastBeatMs);
        lastBeatMs = nowMs;
        handleBeat(instBpm);
      }
    }

    irPrevFiltered = irFiltered;
    redPrevFiltered = redFiltered;

    static unsigned long lastStatus = 0;
    if (millis() - lastStatus >= 1000) {
      lastStatus = millis();
      String bpmText = calibrating ? "calibrating(" + String((nowMs - sessionStartMs) / 1000) + "s)"
                                   : (beatsCollected > 0 ? String(beatAvg) : String("--"));
      String status = "status: ir=" + String(irRaw) + " avgBpm=" + bpmText +
                       " led=" + String(ledBrightness);
      logLine(status);
    }
  }
}
