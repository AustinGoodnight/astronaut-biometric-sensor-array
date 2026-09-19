#include <Wire.h>
#include <string.h>
#include <WiFi.h>
#include "MAX30105.h"

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#error "Missing include/wifi_secrets.h - copy include/wifi_secrets.h.example and fill in your network credentials."
#endif

#define SDA_PIN 22  // XIAO ESP32C6 default I2C (D4)
#define SCL_PIN 23  // XIAO ESP32C6 default I2C (D5)

// Same pipeline as firmware/hr-testing's main_filtered.cpp (bandpass filter ->
// refractory-gated peak detection -> Red/IR cross-check -> median), but streamed over
// WiFi TCP instead of BLE - see that project's CLAUDE.md for why: on macOS, an app can
// only *request* a BLE connection interval, not force one, and CoreBluetooth sometimes
// grants a slow one with no visibility into why, silently starving a high-rate stream.
// TCP has no equivalent per-connection black box, so throughput here is deterministic.
byte ledBrightness = 0x1F;

MAX30105 particleSensor;
bool sensorReady = false;

const uint16_t LOG_PORT = 3333; // human-readable status/beat text, one client at a time
const uint16_t RAW_PORT = 3334; // binary raw IR/Red sample stream, one client at a time

WiFiServer logServer(LOG_PORT);
WiFiServer rawServer(RAW_PORT);
WiFiClient logClient;
WiFiClient rawClient;

// Accepts a new client on `server`, replacing whatever was previously connected on
// `client` - this firmware only supports one viewer per stream at a time, matching the
// hr-testing BLE scripts' single-central model.
void acceptNewClient(WiFiServer &server, WiFiClient &client) {
  if (!server.hasClient()) return;
  if (client.connected()) client.stop();
  client = server.accept();
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());

  logServer.begin();
  rawServer.begin();
  Serial.print("Log stream on port ");
  Serial.print(LOG_PORT);
  Serial.print(", raw sample stream on port ");
  Serial.println(RAW_PORT);
}

// Every log line goes to both USB serial (for a wired monitor) and the log TCP client
// (for the wireless logger) so neither transport requires the other to be connected.
void logLine(const String &line) {
  Serial.println(line);
  if (logClient.connected()) {
    logClient.println(line);
  }
}

// Packed, not a struct-of-arrays: sent byte-for-byte over the socket, so layout must be
// exact (no compiler-inserted padding) and match the Python side's struct.unpack format.
#pragma pack(push, 1)
struct RawSample {
  uint32_t ir;
  uint32_t red;
};
#pragma pack(pop)

// Batch size is no longer MTU-constrained like the BLE version was - TCP handles
// fragmentation/reassembly transparently - but batching still cuts the number of
// write() calls and keeps the wire format identical to the BLE raw stream's, so the
// Python-side unpacking code carries over almost unchanged.
const byte RAW_BATCH_SIZE = 24;
RawSample rawBatch[RAW_BATCH_SIZE];
byte rawBatchCount = 0;
uint16_t rawSeq = 0;

void sendRawSample(uint32_t irRaw, uint32_t redRaw) {
  rawBatch[rawBatchCount].ir = irRaw;
  rawBatch[rawBatchCount].red = redRaw;
  rawBatchCount++;

  if (rawBatchCount < RAW_BATCH_SIZE) return;
  rawBatchCount = 0;

  if (!rawClient.connected()) return;
  uint8_t packet[2 + sizeof(rawBatch)];
  memcpy(packet, &rawSeq, 2);
  memcpy(packet + 2, rawBatch, sizeof(rawBatch));
  rawSeq++;

  rawClient.write(packet, sizeof(packet));
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

const unsigned long REFRACTORY_MS = 300; // caps at 200 bpm; blocks double-triggering on
                                          // the dicrotic notch or filter ripple
unsigned long lastBeatMs = 0;

const byte RATE_SIZE = 9; // median filter window (odd, so the median is a real sample)
byte rates[RATE_SIZE];
byte rateSpot = 0;
byte beatsCollected = 0;
int beatAvg;

void setup() {
  Serial.begin(115200);
  delay(2000); // give the serial monitor a moment to connect

  setupWiFi();
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

  logLine("MAX30102 init succeeded (bandpass + refractory + dual-channel). Place a finger on the sensor.");
  lastBeatMs = millis();
  sensorReady = true;
}

// Same idea as hr-testing/main.cpp: keep the raw IR reading out of both the noise floor
// and ADC saturation (262143), since either one flattens the pulsatile signal the same way.
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

void handleBeat(float instBpm) {
  String line = "beat: " + String(instBpm, 1) + " bpm -> ";

  if (instBpm <= 42 || instBpm >= 220) {
    logLine(line + "REJECTED (outside human range)");
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
  beatAvg = sorted[beatsCollected / 2];

  logLine(line + "KEPT (median now " + String(beatAvg) + ")");
}

void loop() {
  acceptNewClient(logServer, logClient);
  acceptNewClient(rawServer, rawClient);

  if (!sensorReady) {
    tryInitSensor();
    delay(1000);
    return;
  }

  long irRaw = particleSensor.getIR();
  long redRaw = particleSensor.getRed();
  autoAdjustLed(irRaw);
  sendRawSample(irRaw, redRaw);

  float irFiltered = irFilter.process(irRaw);
  float redFiltered = redFilter.process(redRaw);
  unsigned long nowMs = millis();

  bool risingEdge = (irPrevFiltered <= 0) && (irFiltered > 0);
  if (risingEdge && (nowMs - lastBeatMs) >= REFRACTORY_MS) {
    // A real pulse shows up as a (wavelength-dependent) absorption change on both LEDs at
    // essentially the same instant; single-channel noise usually won't. Requiring Red to
    // also be rising right now is a cheap way to reject those without full correlation math.
    bool redCorroborates = (redFiltered - redPrevFiltered) > 0;
    if (!redCorroborates) {
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
    if (irRaw < 50000) {
      logLine("No finger detected");
    } else {
      String status = "status: ir=" + String(irRaw) +
                       " avgBpm=" + (beatsCollected > 0 ? String(beatAvg) : String("--")) +
                       " led=" + String(ledBrightness);
      logLine(status);
    }
  }
}
