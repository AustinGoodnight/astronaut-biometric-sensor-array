#include "BleLink.h"

#include <NimBLEDevice.h>

#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_CHAR_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_CHAR_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

namespace BleLink {

namespace {

const size_t CMD_MAX_LEN = 96;
struct Command {
  char text[CMD_MAX_LEN];
};

NimBLECharacteristic *txChar = nullptr;
QueueHandle_t commandQueue = nullptr;
volatile bool isConnected = false;
// Notify payload is MTU - 3 bytes. Starts at the BLE default (23) until the
// phone negotiates more; iOS usually asks for ~185 right after connecting.
volatile uint16_t notifyChunk = 20;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server, NimBLEConnInfo &connInfo) override {
    isConnected = true;
    notifyChunk = connInfo.getMTU() > 3 ? connInfo.getMTU() - 3 : 20;
  }
  void onDisconnect(NimBLEServer *server, NimBLEConnInfo &connInfo, int reason) override {
    isConnected = false;
    notifyChunk = 20;
    server->getAdvertising()->start(); // keep advertising so the phone can reconnect
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo &connInfo) override {
    notifyChunk = mtu > 3 ? mtu - 3 : 20;
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &connInfo) override {
    std::string value = characteristic->getValue();
    Command cmd;
    size_t len = value.length() < CMD_MAX_LEN - 1 ? value.length() : CMD_MAX_LEN - 1;
    memcpy(cmd.text, value.data(), len);
    cmd.text[len] = '\0';
    xQueueSend(commandQueue, &cmd, 0); // drop if loop() is that far behind
  }
};

}  // namespace

void begin(const char *name) {
  commandQueue = xQueueCreate(4, sizeof(Command));

  NimBLEDevice::init(name);
  NimBLEDevice::setMTU(247);

  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService *service = server->createService(NUS_SERVICE_UUID);
  txChar = service->createCharacteristic(NUS_TX_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic *rxChar = service->createCharacteristic(
      NUS_RX_CHAR_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rxChar->setCallbacks(new RxCallbacks());
  service->start();

  // Name + 128-bit service UUID fit in one advertising packet (30 of 31
  // bytes with a 7-char name), so the app can filter on either.
  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->setName(name);
  advertising->addServiceUUID(NUS_SERVICE_UUID);
  advertising->start();
}

void logLine(const String &line) {
  Serial.println(line);
  if (!isConnected) return;

  String framed = line + "\n";
  const uint8_t *data = reinterpret_cast<const uint8_t *>(framed.c_str());
  size_t remaining = framed.length();
  size_t chunk = notifyChunk;
  while (remaining > 0) {
    size_t n = remaining < chunk ? remaining : chunk;
    txChar->notify(data, n);
    data += n;
    remaining -= n;
  }
}

bool pollCommand(String &out) {
  Command cmd;
  if (commandQueue == nullptr || xQueueReceive(commandQueue, &cmd, 0) != pdTRUE) return false;
  out = cmd.text;
  out.trim();
  return out.length() > 0;
}

bool connected() {
  return isConnected;
}

}  // namespace BleLink
