#pragma once

#include <Arduino.h>

// "BLE serial" for the tx/rx boards, so the range-test web app (Bluefy on
// iOS, see ../../range-app/) can do everything the USB config UI does.
//
// Uses the Nordic UART Service layout (same UUIDs as hr-testing):
//   6E400003 TX: board -> phone, notify. Every logLine() goes here, as
//                newline-terminated text split into MTU-sized chunks; the
//                app reassembles lines on '\n'.
//   6E400002 RX: phone -> board, write. Same "SET ..."/"GET" commands as
//                USB serial, one command per write.
namespace BleLink {

// Starts advertising as `name` (e.g. "LoRa-RX").
void begin(const char *name);

// Prints `line` to USB serial and, if a phone is connected, notifies it.
void logLine(const String &line);

// Returns true and fills `out` if a command arrived over BLE since the
// last call. Writes are queued from the BLE task and handed over here so
// the radio is only ever touched from loop().
bool pollCommand(String &out);

bool connected();

}  // namespace BleLink
