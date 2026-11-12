#include <Arduino.h>
#include <NimBLEDevice.h>

// Minimal isolation test: no sensor code at all, just BLE bring-up with a serial print
// after every single call, so a crash/hang pinpoints exactly which step failed instead
// of leaving us guessing from total silence. This version uses the standalone
// NimBLE-Arduino library instead of the framework's bundled BLEDevice API, since that
// one's advertising never actually went over the air on this board (confirmed with
// three independent scanners) despite every call reporting success.
#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_CHAR_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

int counter = 0;

void setup() {
  Serial.begin(115200);
  delay(2000); // give the serial monitor a moment to connect
  Serial.println("1: Serial up, starting NimBLE bring-up");

  NimBLEDevice::init("HR-Testing");
  Serial.println("2: NimBLEDevice::init done");

  NimBLEServer *server = NimBLEDevice::createServer();
  Serial.println("3: createServer done");

  NimBLEService *service = server->createService(NUS_SERVICE_UUID);
  Serial.println("4: createService done");

  service->createCharacteristic(NUS_TX_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);
  Serial.println("5: createCharacteristic done");

  service->start();
  Serial.println("6: service->start done");

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(NUS_SERVICE_UUID);
  Serial.println("7: addServiceUUID done");

  advertising->start();
  Serial.println("8: advertising started - BLE bring-up complete");
}

void loop() {
  Serial.print("alive: ");
  Serial.println(counter++);
  delay(1000);
}
