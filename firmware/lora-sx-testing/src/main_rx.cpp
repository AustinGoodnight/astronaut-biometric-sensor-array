#include <Arduino.h>
#include <RadioLib.h>
#include <BleLink.h>

// Wio-SX1262 for XIAO (header board) on a XIAO ESP32C6. SPI uses the
// board's default hardware SPI pins (D8 SCK / D9 MISO / D10 MOSI).
#define LORA_NSS   D4  // SPI chip select
#define LORA_DIO1  D1  // IRQ
#define LORA_RST   D2  // NRST
#define LORA_BUSY  D3  // BUSY
#define LORA_RF_SW D5  // RF switch enable, driven HIGH while receiving

#define LORA_FREQ_MHZ     915.0
#define LORA_TCXO_VOLTAGE 1.8  // Wio-SX1262 has a TCXO powered from DIO3

SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);

// Live-tunable radio settings, changed at runtime via commands from the
// USB config UI (../lora-testing/config-ui) or the BLE range-test app
// (../range-app). Same protocol as the ESP32C3/SX127x boards. See
// handleCommand().
// No TX power here — this board only receives.
int currentSF = 9;
long currentBW = 125000;  // Hz on the wire; RadioLib takes kHz

// Set from the DIO1 interrupt when a packet arrives.
volatile bool rxFlag = false;

ICACHE_RAM_ATTR void onRxDone() {
  rxFlag = true;
}

bool isValidBandwidth(long bw) {
  const long allowed[] = {7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000};
  for (long a : allowed) {
    if (bw == a) return true;
  }
  return false;
}

void printConfig() {
  BleLink::logLine("CFG sf=" + String(currentSF) + " bw=" + String(currentBW));
}

// Applies lines like "SET sf=9,bw=125000" or "GET" to the radio
// immediately, without needing a reflash.
void handleCommand(const String &line) {
  if (line == "GET") {
    printConfig();
    return;
  }

  if (line.startsWith("SET ")) {
    // The SX1262 only accepts modulation changes in standby; resume
    // listening once they're applied.
    radio.standby();

    String rest = line.substring(4);
    int idx = 0;
    while (idx < rest.length()) {
      int comma = rest.indexOf(',', idx);
      String token = (comma == -1) ? rest.substring(idx) : rest.substring(idx, comma);
      int eq = token.indexOf('=');
      if (eq != -1) {
        String key = token.substring(0, eq);
        String val = token.substring(eq + 1);
        key.trim();
        val.trim();

        if (key == "sf") {
          int sf = val.toInt();
          if (sf >= 7 && sf <= 12) { // SF5/6 exist on SX126x but not on the SX127x boards
            currentSF = sf;
            radio.setSpreadingFactor(currentSF);
          }
        } else if (key == "bw") {
          long bw = val.toInt();
          if (isValidBandwidth(bw)) {
            currentBW = bw;
            radio.setBandwidth(currentBW / 1000.0);
          }
        }
        // "pwr" is accepted from the same protocol as the tx board but
        // ignored here, since a receiver has no transmit power to set.
      }
      if (comma == -1) break;
      idx = comma + 1;
    }

    rxFlag = false;
    radio.startReceive();
    printConfig();
  }
}

// Commands can arrive over USB serial or BLE; both feed handleCommand().
void pollCommands() {
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) handleCommand(line);
  }

  String bleLine;
  if (BleLink::pollCommand(bleLine)) handleCommand(bleLine);
}

void setup() {
  Serial.begin(115200);
  // Native USB serial blocks on every print while the host isn't reading
  // (port closed, or the link wedged), which stalls loop() and the radio.
  // Drop output instead of waiting.
  Serial.setTxTimeoutMs(0);
  delay(2000); // give the serial monitor a moment to connect

  BleLink::begin("LoRa-RX");

  int state = radio.begin(LORA_FREQ_MHZ, currentBW / 1000.0, currentSF, 5,
                          RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 10,
                          8, LORA_TCXO_VOLTAGE);
  if (state != RADIOLIB_ERR_NONE) {
    // Keep repeating so a phone that connects later still sees why.
    while (1) {
      BleLink::logLine("LoRa init failed, code " + String(state) + ". Check your connections.");
      delay(2000);
    }
  }

  // DIO2 drives the module's TX path; RF_SW is toggled by RadioLib (HIGH in RX).
  radio.setDio2AsRfSwitch(true);
  radio.setRfSwitchPins(LORA_RF_SW, RADIOLIB_NC);

  radio.setPacketReceivedAction(onRxDone);

  state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    while (1) {
      BleLink::logLine("startReceive failed, code " + String(state));
      delay(2000);
    }
  }

  BleLink::logLine("LoRa init succeeded. Starting receiver.");
  printConfig();
}

unsigned long lastHeartbeat = 0;

void loop() {
  pollCommands();

  if (rxFlag) {
    rxFlag = false;

    String packet;
    int state = radio.readData(packet);

    if (state == RADIOLIB_ERR_NONE) {
      BleLink::logLine("Received packet: " + packet + " | RSSI: " + String(radio.getRSSI()) +
                       " | SNR: " + String(radio.getSNR()));
    } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
      BleLink::logLine("Received packet with bad CRC, dropped.");
    } else {
      BleLink::logLine("readData failed, code " + String(state));
    }
  }

  if (millis() - lastHeartbeat >= 1000) {
    lastHeartbeat = millis();
    BleLink::logLine("Heartbeat: listening...");
  }
}
