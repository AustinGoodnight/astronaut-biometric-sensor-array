#include <SPI.h>
#include <LoRa.h>

#define SS   20  // CS
#define RST  2   // RST
#define DIO0 3   // G0/DIO0

void setup() {
  Serial.begin(115200);
  delay(2000); // give the serial monitor a moment to connect

  LoRa.setPins(SS, RST, DIO0);

  if (!LoRa.begin(915E6)) {
    Serial.println("LoRa init failed. Check your connections.");
    while (1);
  }

  LoRa.setSpreadingFactor(9);
  LoRa.setSignalBandwidth(125E3);

  Serial.println("LoRa init succeeded. Starting receiver.");
}

void loop() {
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    Serial.print("Received packet: ");

    while (LoRa.available()) {
      Serial.print((char)LoRa.read());
    }

    Serial.print(" | RSSI: ");
    Serial.println(LoRa.packetRssi());
  }
}
