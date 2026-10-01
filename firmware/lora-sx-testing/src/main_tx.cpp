#include <Arduino.h>
#include <RadioLib.h>

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

// Live-tunable radio settings, changed at runtime via serial commands
// from the host-side config UI (../lora-testing/config-ui). Same protocol
// as the ESP32C3/SX127x boards. See handleSerialCommands().
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
  Serial.print("CFG sf=");
  Serial.print(currentSF);
  Serial.print(" bw=");
  Serial.print(currentBW);
  Serial.print(" pwr=");
  Serial.println(currentPower);
}

// The SX1262 only accepts modulation changes in standby, so abort any
// packet that's still on air before applying new settings.
void enterStandbyForConfig() {
  if (txInProgress) {
    radio.finishTransmit();
    txInProgress = false;
    txDoneFlag = false;
    Serial.println("In-flight packet aborted for config change.");
  }
  radio.standby();
}

// Parses lines like "SET sf=9,bw=125000,pwr=17" or "GET" from Serial and
// applies them to the radio immediately, without needing a reflash.
void handleSerialCommands() {
  if (!Serial.available()) return;

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

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

void setup() {
  Serial.begin(115200);
  delay(2000); // give the serial monitor a moment to connect

  int state = radio.begin(LORA_FREQ_MHZ, currentBW / 1000.0, currentSF, 5,
                          RADIOLIB_SX126X_SYNC_WORD_PRIVATE, currentPower,
                          8, LORA_TCXO_VOLTAGE);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("LoRa init failed, code ");
    Serial.print(state);
    Serial.println(". Check your connections.");
    while (1);
  }

  // DIO2 drives the module's TX path; RF_SW is toggled by RadioLib (HIGH in RX).
  radio.setDio2AsRfSwitch(true);
  radio.setRfSwitchPins(LORA_RF_SW, RADIOLIB_NC);
  //Lowering power is the main way to reduce power consumption. Power is in dBm
  //Likely 10-14 is fine, which is sigifigantly lower than 17 (dB is log)

  radio.setPacketSentAction(onTxDone);

  Serial.println("LoRa init succeeded. Starting transmitter.");
  printConfig();
}

unsigned long lastSend = 0;

void loop() {
  handleSerialCommands();

  if (txDoneFlag) {
    txDoneFlag = false;
    txInProgress = false;
    radio.finishTransmit();
  }

  // Async transmit: don't block loop() (and handleSerialCommands()) for the
  // packet's full time-on-air, which can be many seconds at low
  // bandwidth / high spreading factor. Only one packet on air at a time;
  // lastSend/counter only advance on an actual send.
  if (!txInProgress && millis() - lastSend >= 2000) {
    String packet = "temp:24.5,hum:60,count:" + String(counter);

    int state = radio.startTransmit(packet);
    if (state == RADIOLIB_ERR_NONE) {
      lastSend = millis();
      txInProgress = true;

      Serial.print("Sending packet: ");
      Serial.println(counter);

      counter++;
    } else {
      Serial.print("startTransmit failed, code ");
      Serial.println(state);
      lastSend = millis(); // back off before retrying
    }
  }
}
