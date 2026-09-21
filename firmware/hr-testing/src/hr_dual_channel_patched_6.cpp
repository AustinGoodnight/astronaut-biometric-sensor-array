#include <Wire.h>
#include <string.h>
#include <math.h>
#include "MAX30105.h"
#include <NimBLEDevice.h>

#define SDA_PIN 22  // XIAO ESP32C6 default I2C (D4)
#define SCL_PIN 23  // XIAO ESP32C6 default I2C (D5)

// ============================================================================
// PATCHED VERSION - what changed vs. the original, and why
//
//  1. autoAdjustLed() is GONE. It raised IR LED current ~12% whenever a single raw IR
//     sample fell between 50k and 80k - but your healthy DC level is 82-94k, so normal
//     beat valleys triggered it. Every step was 3-5x bigger than a pulse and only hit
//     the IR channel. The LED current is now fixed (LED_CURRENT). At 0x1F your finger DC
//     is ~1/3 of full scale, which is fine. If you ever want auto-gain, do it ONCE at
//     session start, then freeze it.
//  2. Beat detection now triggers on the FALLING edge of the filtered IR. In reflective
//     mode a heartbeat is a sharp dip, so the falling edge is the steep, clean part of
//     the pulse; the old rising edge sat on the slow recovery slope where the dicrotic
//     bump caused extra crossings. Red corroboration flipped to match (Red must also fall).
//  3. Refractory period is adaptive (65% of the current beat period, floor 300 ms), and
//     a rejected/false edge no longer becomes the timing reference for the next interval.
//  4. Filters are proper 2nd-order Butterworth biquads (0.7-3.5 Hz) instead of two weak
//     single-pole filters.
//  5. Heart rate now comes from a SLIDING 8 s AUTOCORRELATION run once per second (your
//     calibration idea, made continuous). Individual edges are only used as beat markers
//     and are validated against that estimate. This replaces the one-shot 30 s calibration.
//  6. Artifact handling: a sample-to-sample jump > 3% of DC (finger moved/removed, LED
//     glitch) resets the filters and the window and holds off detection for 1.5 s.
//     A signal-quality gate (AC/DC ratio) and a saturation check suppress output when
//     there's no usable pulse.
//  8. The autocorrelation window is decimated to 25 Hz (see WIN_LEN) - the C6 has no FPU,
//     and a full-rate estimate was blocking long enough to overflow the sensor FIFO.
//  7. All timing uses SAMPLE COUNT (÷ measured sample rate) instead of millis(), so
//     bursty FIFO reads / BLE / Serial delays can't distort beat intervals.
// ============================================================================

// LED current (register value, ~0.2 mA per step). Boot default 0x2F ~ 9.4 mA (was 0x1F ~ 6.2 mA;
// raised to improve SNR on a weak pulse). Never adjusted automatically; the only way it changes
// is an explicit 'L<0-255>\n' command over USB serial (the dashboard's LED slider).
// Aim for finger DC roughly 30-70% of 262143; if IR DC on a finger exceeds ~200k, back this off.
const byte LED_CURRENT_DEFAULT = 0x2F;
byte ledCurrent = LED_CURRENT_DEFAULT;

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
bool serialRawOn = false; // set by 'R' / cleared by 'r' received over USB serial

// Buffers one sample per loop iteration and flushes a batch once full, rather than
// notifying per-sample - cuts the notify() call rate ~24x, which matters because each
// call still costs a full BLE connection event even with the tightened interval above.
void sendRawSample(uint32_t irRaw, uint32_t redRaw) {
  rawBatch[rawBatchCount].ir = irRaw;
  rawBatch[rawBatchCount].red = redRaw;
  rawBatchCount++;

  if (rawBatchCount < RAW_BATCH_SIZE) return;
  rawBatchCount = 0;
  uint16_t seq = rawSeq++;

  // USB path (dashboard over Web Serial): one text line per batch, only after the host has
  // sent 'R'. Format: "raw,<seq>,<ir>:<red>,<ir>:<red>,..." (24 pairs). Kept as text so it
  // coexists with the log lines on the same port. TX timeout is 0 (see setup) so a slow or
  // absent host drops data instead of stalling the sample loop.
  if (serialRawOn) {
    char line[16 + RAW_BATCH_SIZE * 16];
    int n = snprintf(line, sizeof(line), "raw,%u", (unsigned)seq);
    for (byte i = 0; i < RAW_BATCH_SIZE && n < (int)sizeof(line) - 16; i++)
      n += snprintf(line + n, sizeof(line) - n, ",%lu:%lu",
                    (unsigned long)rawBatch[i].ir, (unsigned long)rawBatch[i].red);
    line[n++] = '\n';
    Serial.write((const uint8_t *)line, n);
  }

  if (!bleConnected) return;
  uint8_t packet[2 + sizeof(rawBatch)];
  memcpy(packet, &seq, 2);
  memcpy(packet + 2, rawBatch, sizeof(rawBatch));

  rawCharacteristic->setValue(packet, sizeof(packet));
  rawCharacteristic->notify();
}

// ============================================================================
// Signal pipeline
// ============================================================================

const float FS_NOMINAL = 100.0f;  // 400 sps / 4x on-chip averaging
float fsHz = FS_NOMINAL;          // refined from real sample count once a session is >10 s old

// --- 2nd-order Butterworth bandpass, 0.7-3.5 Hz @ 100 Hz (two cascaded biquads) ---
// Coefficients from scipy.signal.butter(2, [0.7, 3.5], 'bandpass', fs=100, output='sos').
// They assume fs=100; a few % of sample-rate error just shifts the band edges slightly.
struct Biquad {
  float b0, b1, b2, a1, a2;
  float z1 = 0, z2 = 0;
  float process(float x) { // transposed direct form II
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
  void reset() { z1 = z2 = 0; }
};

struct PpgFilter {
  Biquad s0, s1;
  PpgFilter() {
    s0.b0 = 0.006867866f; s0.b1 = 0.013735731f; s0.b2 = 0.006867866f;
    s0.a1 = -1.786023501f; s0.a2 = 0.820363944f;
    s1.b0 = 1.0f; s1.b1 = -2.0f; s1.b2 = 1.0f;
    s1.a1 = -1.948065846f; s1.a2 = 0.950479924f;
  }
  float process(float x) { return s1.process(s0.process(x)); }
  void reset() { s0.reset(); s1.reset(); }
};

PpgFilter irFilter;
PpgFilter redFilter;

// Slow DC estimates, subtracted BEFORE the bandpass so the filters never see the ~85k DC
// as a giant startup step. Also used as the denominator of the AC/DC quality ratio.
const float DC_ALPHA = 0.01f;   // ~1 s time constant at 100 sps
float irDc = 0, redDc = 0;
float envAbs = 0;               // EMA of |filtered IR| ~ pulse amplitude
const float ENV_ALPHA = 0.02f;

float prevIrRaw = 0;
float irPrevFiltered = 0;
float redPrevFiltered = 0;
float redSlopeAvg = 0;          // smoothed Red slope, single-pole (see corroboration check)

// --- Tunables ---
const float MIN_ACDC = 0.0003f;         // edge-detector gate only (mean|AC|/DC). A good finger signal is ~0.5-1%;
                                        // the autocorrelation r value is the real quality test
const float STEP_FRAC = 0.03f;          // sample-to-sample jump > 3% of DC = artifact (real beats ~0.5%)
const float HOLDOFF_STEP_SEC = 1.5f;    // detection pause after an artifact
const float SETTLE_SEC = 2.0f;          // detection pause at session start
const float SLOPE_GATE = 0.02f;         // falling edge must drop >= 2% of envelope in one sample (noise guard only)
const float REFRACTORY_MIN_SEC = 0.30f; // caps at 200 bpm
const float REFRACTORY_FRAC = 0.65f;    // ...but never less than 65% of the current beat period
const long  SAT_LEVEL = 250000;         // ADC saturates at 262143; treat this as "too bright"

// --- Session / timing state (everything measured in samples since session start) ---
bool sessionActive = false;
unsigned long sessionStartMs = 0;
unsigned long sampleIdx = 0;
unsigned long holdoffUntil = 0;
unsigned long lastTickSample = 0;
bool needInit = true;

// --- Sliding-window autocorrelation heart rate ---
// The window is DECIMATED 4x (100 -> 25 Hz) before autocorrelation. The C6 has no FPU, so
// float math is software-emulated; a full-rate 8 s autocorrelation took long enough to
// overflow the sensor's 32-sample FIFO and silently drop samples (seen as measured fs
// sagging below 100 in the status line). At 25 Hz it's ~16x cheaper, and the parabolic
// peak interpolation keeps the BPM resolution fine. The bandpass already removed
// everything above 3.5 Hz, so there's nothing to alias.
const int WIN_DECIM = 4;
const int WIN_LEN = 200;                // 8 s @ 25 Hz
const int MAX_LAG = 40;                 // covers 42 bpm (lag ~36 @ 25 Hz) with margin
const float HR_JUMP_BPM = 15.0f;        // a window this far from the published value must persist...
const byte HR_JUMP_CONFIRM = 3;         // ...for this many consecutive windows (within 10 bpm of each other)
const byte HR_FAIL_TICKS = 5;           // this many failed windows in a row clears the reading
const float AC_MIN_CORR = 0.50f;        // windows less periodic than this are not published (was 0.30)
const byte HR_HIST = 5;                 // median over the last 5 estimates
const float HR_STALE_SEC = 15.0f;       // drop the reading if no valid estimate for this long
float winBuf[WIN_LEN];
int winHead = 0, winCount = 0;
float decAcc = 0;                       // running sum for 4x decimation
byte decCount = 0;
float lastR = 0;                        // autocorrelation peak of the last window (0..1, want > ~0.5)
unsigned long acUs = 0;                 // how long the last estimate took
byte failTicks = 0;
float pendingBpm = 0;                   // candidate for a big jump, awaiting confirmation
byte pendingCount = 0;
float hrHist[HR_HIST];
byte hrHistSpot = 0, hrHistCount = 0;
float hrEstimate = 0;
bool hrValid = false;
unsigned long lastHrSample = 0;

// --- Edge-based beat markers (validated against hrEstimate) ---
bool haveLastBeat = false;
unsigned long lastBeatSample = 0;
const byte RATE_SIZE = 9;
byte rates[RATE_SIZE];
byte rateSpot = 0;
byte beatsCollected = 0;
int beatMedian = 0;

// A session is started/stopped with a button, instead of trying to infer finger presence
// from the signal. Everything that happens outside a session (sensor on the desk, finger
// being placed/removed) is ignored, so it can't pollute the beat history or estimates.
// External momentary button on D0 (GPIO0), other leg to GND; uses the internal pull-up, so
// no resistor needed. (The onboard BOOT button on GPIO9 also works if you set this to 9.)
#define BUTTON_PIN 0
const unsigned long BUTTON_DEBOUNCE_MS = 50;

void startSession();
void stopSession();

void setup() {
  Serial.setTxBufferSize(4096); // must precede begin(); holds ~2 s of raw lines if the host stalls
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);     // never block the sample loop on USB
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
  particleSensor.setup(ledCurrent, 4, 2, 400, 411, 4096);

  logLine("MAX30102 init succeeded (fixed LED, falling-edge + sliding autocorrelation). Press the button to start a session.");
  sensorReady = true;
}

// ---------------------------------------------------------------------------
// Autocorrelation heart-rate estimate over the current window.
// A periodic pulse correlates strongly with itself shifted by one beat period. Lags are
// searched only over the human range (42-220 bpm), and the *first* strong peak is taken
// rather than the global max, since multiples of the true period (half the bpm) correlate
// almost as well. The peak is refined with parabolic interpolation (1 sample ~ 1.5 bpm).
// Autocorrelation is polarity-independent, so no inversion is needed here.
// ---------------------------------------------------------------------------
bool estimateHr(float &bpmOut, float &rOut) {
  static float x[WIN_LEN];
  static float ac[MAX_LAG + 2];
  int n = winCount;
  int start = (winHead - n + WIN_LEN) % WIN_LEN; // oldest sample in the ring

  float mean = 0;
  for (int i = 0; i < n; i++) {
    x[i] = winBuf[(start + i) % WIN_LEN];
    mean += x[i];
  }
  mean /= n;
  float r0 = 0;
  for (int i = 0; i < n; i++) {
    x[i] -= mean;
    r0 += x[i] * x[i];
  }
  r0 /= n;
  if (r0 <= 0) return false;

  float fsWin = fsHz / WIN_DECIM;
  int lagMin = (int)(fsWin * 60.0f / 220.0f);
  int lagMax = (int)(fsWin * 60.0f / 42.0f + 0.5f);
  if (lagMin < 2) lagMin = 2;
  if (lagMax > MAX_LAG - 1) lagMax = MAX_LAG - 1;

  for (int lag = lagMin - 1; lag <= lagMax + 1; lag++) {
    float sum = 0;
    for (int i = 0; i + lag < n; i++) sum += x[i] * x[i + lag];
    ac[lag] = (sum / (n - lag)) / r0; // normalized: 1.0 = perfectly periodic at this lag
  }

  float rmax = 0;
  for (int lag = lagMin; lag <= lagMax; lag++) rmax = max(rmax, ac[lag]);
  rOut = rmax;
  if (rmax < AC_MIN_CORR) return false;

  int best = -1;
  for (int lag = lagMin; lag <= lagMax; lag++) {
    if (ac[lag] >= ac[lag - 1] && ac[lag] >= ac[lag + 1] && ac[lag] >= 0.85f * rmax) {
      best = lag;
      break;
    }
  }
  if (best < 0) return false;

  float a = ac[best - 1], b = ac[best], c = ac[best + 1];
  float denom = a - 2.0f * b + c;
  float delta = (denom != 0.0f) ? 0.5f * (a - c) / denom : 0.0f;
  if (delta > 1.0f) delta = 1.0f;
  if (delta < -1.0f) delta = -1.0f;
  bpmOut = 60.0f * fsWin / ((float)best + delta);
  return true;
}

// Median of the last few window estimates, so one odd window can't move the reading.
void publishHr(float bpm) {
  // Jump guard: a real HR change of >15 bpm doesn't happen in one second. A window that
  // disagrees that much with the published value is held back until it repeats.
  if (hrValid && fabsf(bpm - hrEstimate) > HR_JUMP_BPM) {
    if (pendingCount > 0 && fabsf(bpm - pendingBpm) <= 10.0f) pendingCount++;
    else pendingCount = 1;
    pendingBpm = bpm;
    if (pendingCount < HR_JUMP_CONFIRM) return; // hold the current reading
    hrHistCount = 0;                            // persisted: adopt it, restart the median
    hrHistSpot = 0;
  }
  pendingCount = 0;

  hrHist[hrHistSpot++] = bpm;
  hrHistSpot %= HR_HIST;
  if (hrHistCount < HR_HIST) hrHistCount++;

  float tmp[HR_HIST];
  memcpy(tmp, hrHist, hrHistCount * sizeof(float));
  for (byte i = 1; i < hrHistCount; i++) {
    float key = tmp[i];
    int j = i - 1;
    while (j >= 0 && tmp[j] > key) {
      tmp[j + 1] = tmp[j];
      j--;
    }
    tmp[j + 1] = key;
  }
  hrEstimate = tmp[hrHistCount / 2];
  hrValid = true;
  lastHrSample = sampleIdx;
}

void clearHr() {
  pendingCount = 0;
  hrValid = false;
  hrHistCount = 0;
  hrHistSpot = 0;
}

// ---------------------------------------------------------------------------
// Beat markers. Returns true if this edge should become the timing reference for the
// NEXT interval. Edges that are clearly false (too fast) do not, so a notch or noise
// spike can't corrupt the following interval. Edges that follow a missed beat DO
// (resync), but aren't scored.
// ---------------------------------------------------------------------------
bool handleBeat(float instBpm) {
  String line = "beat: " + String(instBpm, 1) + " bpm -> ";

  if (!hrValid) {
    logLine(line + "unscored (waiting for window estimate)");
    return true;
  }

  float ratio = instBpm / hrEstimate;
  if (ratio > 1.25f) {
    logLine(line + "IGNORED (too fast vs " + String((int)(hrEstimate + 0.5f)) + " - likely false edge)");
    return false;
  }
  if (ratio < 0.75f) {
    logLine(line + "RESYNC (too slow vs " + String((int)(hrEstimate + 0.5f)) + " - likely missed beat)");
    return true;
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
  beatMedian = sorted[beatsCollected / 2];

  logLine(line + "KEPT (beat median " + String(beatMedian) + ", window est " +
          String((int)(hrEstimate + 0.5f)) + ")");
  return true;
}

void onEdge() {
  if (!haveLastBeat) { // first edge after start/artifact: timing reference only
    lastBeatSample = sampleIdx;
    haveLastBeat = true;
    return;
  }

  unsigned long interval = sampleIdx - lastBeatSample;
  float minInterval = REFRACTORY_MIN_SEC * fsHz;
  if (hrValid) minInterval = max(minInterval, REFRACTORY_FRAC * 60.0f * fsHz / hrEstimate);
  if ((float)interval < minInterval) return; // inside refractory: silently ignore

  // A real pulse shows up as an absorption change on both LEDs at essentially the same
  // instant; single-channel noise usually won't. The beat is a dip, so Red must also be
  // falling. Smoothed slope rides through Red's smaller/noisier swing.
  if (redSlopeAvg >= 0) {
    logLine("candidate beat REJECTED (no Red-channel corroboration)");
    return;
  }

  float instBpm = 60.0f * fsHz / (float)interval;
  if (handleBeat(instBpm)) lastBeatSample = sampleIdx;
}

// Drop filter/window state after an artifact (finger moved/removed, etc.) so its
// transient can't masquerade as beats or poison the autocorrelation window.
void startHoldoff(float irRaw, float redRaw, float seconds) {
  irFilter.reset();
  redFilter.reset();
  irDc = irRaw;
  redDc = redRaw;
  irPrevFiltered = redPrevFiltered = redSlopeAvg = 0;
  winHead = winCount = 0;
  decAcc = 0; decCount = 0;
  haveLastBeat = false;
  holdoffUntil = sampleIdx + (unsigned long)(seconds * fsHz);
}

// Once a second: refine the real sample rate, run the window estimate, print status.
void onTick(long irRaw) {
  unsigned long elapsedMs = millis() - sessionStartMs;
  if (elapsedMs >= 10000) {
    float est = sampleIdx * 1000.0f / elapsedMs;
    if (est > 90.0f && est < 110.0f) fsHz = est; // ignore nonsense (e.g. after a stall)
  }

  if (hrValid && (float)(sampleIdx - lastHrSample) > HR_STALE_SEC * fsHz) clearHr();

  if (winCount >= WIN_LEN) {
    float bpm;
    unsigned long t0 = micros();
    bool ok = estimateHr(bpm, lastR);
    acUs = micros() - t0;
    if (ok) {
      publishHr(bpm);
      failTicks = 0;
    } else {
      if (failTicks < 255) failTicks++;
      if (failTicks >= HR_FAIL_TICKS && hrValid) { // finger off / no periodic pulse
        clearHr();
        logLine("no periodic pulse for " + String(HR_FAIL_TICKS) + " windows - reading cleared");
      }
    }
  }

  float acdc = (irDc > 0) ? envAbs / irDc : 0;
  String bpmText;
  if (hrValid) bpmText = String((int)(hrEstimate + 0.5f));
  else if (winCount < WIN_LEN) bpmText = "warming(" + String((int)(winCount * WIN_DECIM / fsHz)) + "/" +
                                          String((int)(WIN_LEN * WIN_DECIM / fsHz)) + "s)";
  else bpmText = "--";

  logLine("status: ir=" + String(irRaw) + " avgBpm=" + bpmText + " led=" + String(ledCurrent) +
          " acdc=" + String(acdc * 100.0f, 2) + "% r=" + String(lastR, 2) + " fs=" + String(fsHz, 1) +
          " est=" + String(acUs / 1000.0f, 1) + "ms");
}

void processSample(long irRaw, long redRaw) {
  float ir = (float)irRaw;
  float red = (float)redRaw;
  sampleIdx++;

  if (needInit) { // first sample of the session
    needInit = false;
    irDc = ir;
    redDc = red;
    prevIrRaw = ir;
    holdoffUntil = (unsigned long)(SETTLE_SEC * fsHz);
  }

  // Artifact detector: a real beat moves ~0.5% of DC per sample; anything > 3% is a
  // step (finger shift/removal, LED or register change), not physiology.
  if (fabsf(ir - prevIrRaw) > STEP_FRAC * irDc) {
    if (sampleIdx >= holdoffUntil) logLine("step artifact detected - resetting filters, holding off");
    // A huge step (>10% of DC) is a finger being removed/placed, not a nudge: the old
    // reading is no longer trustworthy, so blank it and re-warm.
    if (fabsf(ir - prevIrRaw) > 0.10f * irDc && hrValid) clearHr();
    startHoldoff(ir, red, HOLDOFF_STEP_SEC);
  }
  prevIrRaw = ir;

  float irIn = ir - irDc;
  irDc += DC_ALPHA * irIn;
  float redIn = red - redDc;
  redDc += DC_ALPHA * redIn;

  float irF = irFilter.process(irIn);
  float redF = redFilter.process(redIn);
  redSlopeAvg = 0.7f * redSlopeAvg + 0.3f * (redF - redPrevFiltered);

  if (sampleIdx >= holdoffUntil) {
    envAbs += ENV_ALPHA * (fabsf(irF) - envAbs);

    decAcc += irF;
    if (++decCount >= WIN_DECIM) { // one window sample per WIN_DECIM input samples
      winBuf[winHead] = decAcc / WIN_DECIM;
      winHead = (winHead + 1) % WIN_LEN;
      if (winCount < WIN_LEN) winCount++;
      decAcc = 0;
      decCount = 0;
    }

    bool signalOk = irDc > 0 && (envAbs / irDc) >= MIN_ACDC && irRaw < SAT_LEVEL;
    bool fallingEdge = (irPrevFiltered >= 0) && (irF < 0) &&
                       ((irPrevFiltered - irF) > SLOPE_GATE * envAbs);
    if (fallingEdge && signalOk) onEdge();
  }

  irPrevFiltered = irF;
  redPrevFiltered = redF;

  if (sampleIdx - lastTickSample >= (unsigned long)fsHz) {
    lastTickSample = sampleIdx;
    onTick(irRaw);
  }
}

void startSession() {
  irFilter.reset();
  redFilter.reset();
  irPrevFiltered = redPrevFiltered = redSlopeAvg = 0;
  envAbs = 0;
  fsHz = FS_NOMINAL;
  sampleIdx = 0;
  lastTickSample = 0;
  holdoffUntil = 0;
  needInit = true;
  winHead = winCount = 0;
  decAcc = 0; decCount = 0;
  lastR = 0; failTicks = 0; acUs = 0; pendingCount = 0;
  hrHistCount = hrHistSpot = 0;
  hrValid = false;
  hrEstimate = 0;
  haveLastBeat = false;
  beatsCollected = 0;
  rateSpot = 0;
  beatMedian = 0;
  sessionStartMs = millis();
  sessionActive = true;
  logLine("SESSION START - warming up ~10s (2s settle + 8s window), keep finger still");
}

void stopSession() {
  sessionActive = false;
  String summary = "SESSION STOP (" + String((millis() - sessionStartMs) / 1000) + "s, ";
  summary += hrValid ? "final avgBpm=" + String((int)(hrEstimate + 0.5f)) : String("no valid reading");
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
  static bool ledParsing = false;
  static int ledVal = 0;
  if (!sensorReady) {
    tryInitSensor();
    delay(1000);
    return;
  }

  // check() + available()/getFIFOIR()/getFIFORed()/nextSample() is the library's intended
  // pattern for pulling both channels of the SAME sample together (getIR()/getRed() each
  // drain a separate FIFO entry and would desync the channels).
  pollButton();
  while (Serial.available()) {
    int c = Serial.read();
    if (c == 'L') { ledParsing = true; ledVal = 0; }
    else if (ledParsing && c >= '0' && c <= '9') { if (ledVal < 1000) ledVal = ledVal * 10 + (c - '0'); }
    else if (ledParsing) { // any non-digit terminates the number
      ledParsing = false;
      ledCurrent = (byte)min(ledVal, 255);
      particleSensor.setPulseAmplitudeRed(ledCurrent);
      particleSensor.setPulseAmplitudeIR(ledCurrent);
      logLine("LED current set to " + String(ledCurrent) + " (the step detector will hold off briefly)");
    }
    else if (c == 'R') serialRawOn = true;
    else if (c == 'r') serialRawOn = false;
  }
  particleSensor.check();

  while (particleSensor.available()) {
    long irRaw = particleSensor.getFIFOIR();
    long redRaw = particleSensor.getFIFORed();
    particleSensor.nextSample();

    sendRawSample(irRaw, redRaw);

    if (!sessionActive) {
      static unsigned long lastIdleStatus = 0;
      if (millis() - lastIdleStatus >= 1000) {
        lastIdleStatus = millis();
        logLine("Idle - press button to start session");
      }
      continue;
    }

    processSample(irRaw, redRaw);
  }
}
