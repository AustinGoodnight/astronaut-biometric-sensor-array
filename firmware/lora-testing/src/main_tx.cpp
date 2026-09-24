#include <SPI.h>
#include <LoRa.h>

#define SS   20  // CS
#define RST  2   // RST
#define DIO0 3   // G0/DIO0

int counter = 0;

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
  LoRa.setTxPower(17);
  //Lowering power is the main way to reduce power consumption. Power is in dB
  //Likely 10-14 is fine, which is sigifigantly lower than 17 (dB is log)

  Serial.println("LoRa init succeeded. Starting transmitter.");
}

void loop() {
  //should use a timer interupt intead of continous loop in final program for each second
  Serial.print("Sending packet: ");
  Serial.println(counter);

  LoRa.beginPacket();
  LoRa.print("temp:24.5,hum:60,count:");
  LoRa.print(counter);
  LoRa.endPacket();

  counter++;
  delay(2000);
}
