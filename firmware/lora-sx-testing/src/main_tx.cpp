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

int counter = 0;

// Live-tunable radio settings, changed at runtime via commands from the
// USB config UI (../lora-testing/config-ui) or the BLE range-test app
// (../range-app). Same protocol as the ESP32C3/SX127x boards. See
// handleCommand().
int currentSF = 9;
long currentBW = 125000;  // Hz on the wire; RadioLib takes kHz
int currentPower = 17;

// Set from the DIO1 interrupt when an async transmission finishes.
volatile bool txDoneFlag = false;
bool txInProgress = false;

ICACHE_RAM_ATTR void onTxDone() {
  txDoneFlag = true;
}

bool isValidBandwidth(long bw) {
  const long allowed[] = {7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000};
  for (long a : allowed) {
    if (bw == a) return true;
  }
  return false;
}

void printConfig() {
  BleLink::logLine("CFG sf=" + String(currentSF) + " bw=" + String(currentBW) +
                   " pwr=" + String(currentPower));
}

// The SX1262 only accepts modulation changes in standby, so abort any
// packet that's still on air before applying new settings.
void enterStandbyForConfig() {
  if (txInProgress) {
    radio.finishTransmit();
    txInProgress = false;
    txDoneFlag = false;
    BleLink::logLine("In-flight packet aborted for config change.");
  }
  radio.standby();
}

// Applies lines like "SET sf=9,bw=125000,pwr=17" or "GET" to the radio
// immediately, without needing a reflash.
void handleCommand(const String &line) {
  if (line == "GET") {
    printConfig();
    return;
  }

  if (line.startsWith("SET ")) {
    enterStandbyForConfig();

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
        } else if (key == "pwr") {
          int pwr = val.toInt();
          if (pwr >= -9 && pwr <= 22) { // SX1262 PA range
            currentPower = pwr;
            radio.setOutputPower(currentPower);
          }
        }
      }
      if (comma == -1) break;
      idx = comma + 1;
    }
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

  BleLink::begin("LoRa-TX");

  int state = radio.begin(LORA_FREQ_MHZ, currentBW / 1000.0, currentSF, 5,
                          RADIOLIB_SX126X_SYNC_WORD_PRIVATE, currentPower,
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
  //Lowering power is the main way to reduce power consumption. Power is in dBm
  //Likely 10-14 is fine, which is sigifigantly lower than 17 (dB is log)

  radio.setPacketSentAction(onTxDone);

  BleLink::logLine("LoRa init succeeded. Starting transmitter.");
  printConfig();
}

unsigned long lastSend = 0;

void loop() {
  pollCommands();

  if (txDoneFlag) {
    txDoneFlag = false;
    txInProgress = false;
    radio.finishTransmit();
  }

  // Async transmit: don't block loop() (and command handling) for the
  // packet's full time-on-air, which can be many seconds at low
  // bandwidth / high spreading factor. Only one packet on air at a time;
  // lastSend/counter only advance on an actual send.
  if (!txInProgress && millis() - lastSend >= 2000) {
    String packet = "temp:24.5,hum:60,count:" + String(counter);

    int state = radio.startTransmit(packet);
    if (state == RADIOLIB_ERR_NONE) {
      lastSend = millis();
      txInProgress = true;

      BleLink::logLine("Sending packet: " + String(counter));

      counter++;
    } else {
      BleLink::logLine("startTransmit failed, code " + String(state));
      lastSend = millis(); // back off before retrying
    }
  }
}
