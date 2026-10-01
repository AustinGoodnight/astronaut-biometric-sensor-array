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

// Live-tunable radio settings, changed at runtime via serial commands
// from the host-side config UI (../lora-testing/config-ui). Same protocol
// as the ESP32C3/SX127x boards. See handleSerialCommands().
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
  Serial.print("CFG sf=");
  Serial.print(currentSF);
  Serial.print(" bw=");
  Serial.println(currentBW);
}

// Parses lines like "SET sf=9,bw=125000" or "GET" from Serial and applies
// them to the radio immediately, without needing a reflash.
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

void setup() {
  Serial.begin(115200);
  delay(2000); // give the serial monitor a moment to connect

  int state = radio.begin(LORA_FREQ_MHZ, currentBW / 1000.0, currentSF, 5,
                          RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 10,
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

  radio.setPacketReceivedAction(onRxDone);

  state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("startReceive failed, code ");
    Serial.println(state);
    while (1);
  }

  Serial.println("LoRa init succeeded. Starting receiver.");
  printConfig();
}

unsigned long lastHeartbeat = 0;

void loop() {
  handleSerialCommands();

  if (rxFlag) {
    rxFlag = false;

    String packet;
    int state = radio.readData(packet);

    if (state == RADIOLIB_ERR_NONE) {
      Serial.print("Received packet: ");
      Serial.print(packet);
      Serial.print(" | RSSI: ");
      Serial.print(radio.getRSSI());
      Serial.print(" | SNR: ");
      Serial.println(radio.getSNR());
    } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
      Serial.println("Received packet with bad CRC, dropped.");
    } else {
      Serial.print("readData failed, code ");
      Serial.println(state);
    }
  }

  if (millis() - lastHeartbeat >= 1000) {
    lastHeartbeat = millis();
    Serial.println("Heartbeat: listening...");
  }
}
