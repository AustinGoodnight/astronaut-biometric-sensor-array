#include <SPI.h>
#include <LoRa.h>

#define SS   20  // CS
#define RST  2   // RST
#define DIO0 3   // G0/DIO0

// Live-tunable radio settings, changed at runtime via serial commands
// from the host-side config UI (see config-ui/). See handleSerialCommands().
// No TX power here — this board only receives.
int currentSF = 9;
long currentBW = 125E3;

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
          if (sf >= 7 && sf <= 12) { // SF6 needs implicit-header mode, unsupported here
            currentSF = sf;
            LoRa.setSpreadingFactor(currentSF);
          }
        } else if (key == "bw") {
          long bw = val.toInt();
          if (isValidBandwidth(bw)) {
            currentBW = bw;
            LoRa.setSignalBandwidth(currentBW);
          }
        }
        // "pwr" is accepted from the same protocol as the tx board but
        // ignored here, since a receiver has no transmit power to set.
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

  LoRa.setPins(SS, RST, DIO0);

  if (!LoRa.begin(915E6)) {
    Serial.println("LoRa init failed. Check your connections.");
    while (1);
  }

  LoRa.setSpreadingFactor(currentSF);
  LoRa.setSignalBandwidth(currentBW);

  Serial.println("LoRa init succeeded. Starting receiver.");
  printConfig();
}

unsigned long lastHeartbeat = 0;

void loop() {
  handleSerialCommands();

  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    Serial.print("Received packet: ");

    while (LoRa.available()) {
      Serial.print((char)LoRa.read());
    }

    Serial.print(" | RSSI: ");
    Serial.println(LoRa.packetRssi());
  }

  if (millis() - lastHeartbeat >= 1000) {
    lastHeartbeat = millis();
    Serial.println("Heartbeat: listening...");
  }
}
