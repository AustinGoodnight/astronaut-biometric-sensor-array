#include <SPI.h>
#include <LoRa.h>

#define SS   20  // CS
#define RST  2   // RST
#define DIO0 3   // G0/DIO0

int counter = 0;

// Live-tunable radio settings, changed at runtime via serial commands
// from the host-side config UI (see config-ui/). See handleSerialCommands().
int currentSF = 9;
long currentBW = 125E3;
int currentPower = 17;

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
        } else if (key == "pwr") {
          int pwr = val.toInt();
          if (pwr >= 2 && pwr <= 20) {
            currentPower = pwr;
            LoRa.setTxPower(currentPower);
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

  LoRa.setPins(SS, RST, DIO0);

  if (!LoRa.begin(915E6)) {
    Serial.println("LoRa init failed. Check your connections.");
    while (1);
  }

  LoRa.setSpreadingFactor(currentSF);
  LoRa.setSignalBandwidth(currentBW);
  LoRa.setTxPower(currentPower);
  //Lowering power is the main way to reduce power consumption. Power is in dB
  //Likely 10-14 is fine, which is sigifigantly lower than 17 (dB is log)

  Serial.println("LoRa init succeeded. Starting transmitter.");
  printConfig();
}

unsigned long lastSend = 0;

void loop() {
  handleSerialCommands();

  // beginPacket() itself refuses (returns 0) if a previous async transmission
  // is still on air, so this naturally retries next loop() iteration instead
  // of stepping on it. Only advance lastSend/counter on an actual send.
  if (millis() - lastSend >= 2000 && LoRa.beginPacket()) {
    lastSend = millis();

    Serial.print("Sending packet: ");
    Serial.println(counter);

    LoRa.print("temp:24.5,hum:60,count:");
    LoRa.print(counter);
    // async: don't block loop() (and handleSerialCommands()) for the
    // packet's full time-on-air, which can be many seconds at low
    // bandwidth / high spreading factor.
    LoRa.endPacket(true);

    counter++;
  }
}
