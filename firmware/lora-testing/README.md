# LoRa TX/RX Link

A minimal point-to-point LoRa link between two [Seeed XIAO ESP32C3](https://wiki.seeedstudio.com/XIAO_ESP32C3_Getting_Started/)
boards, built with [PlatformIO](https://platformio.org) and the Arduino
framework. One board runs the transmitter firmware, the other the receiver;
the transmitter sends a counter/dummy sensor packet every 2 seconds over a
915 MHz LoRa radio, and the receiver prints each packet plus its RSSI to
serial.

| | |
|---|---|
| **Board**    | Seeed XIAO ESP32C3 |
| **Radio**    | SX127x-class LoRa module (via [`sandeepmistry/LoRa`](https://github.com/sandeepmistry/arduino-LoRa)) |
| **Frequency**| 915 MHz |
| **TX code**  | [src/main_tx.cpp](src/main_tx.cpp) |
| **RX code**  | [src/main_rx.cpp](src/main_rx.cpp) |

## Wiring

Both boards use the same pinout, defined at the top of each `main_*.cpp`:

| Signal | XIAO ESP32C3 pin |
|--------|-------------------|
| SS (CS)  | 20 |
| RST      | 2  |
| DIO0/G0  | 3  |
| SPI (SCK/MISO/MOSI) | default hardware SPI pins |

## Quick start

1. Install [VS Code](https://code.visualstudio.com/) and the
   **pioarduino** extension (`pioarduino.pioarduino-ide`) — see
   [.vscode/extensions.json](.vscode/extensions.json). No separate
   PlatformIO toolchain install is needed; it's fetched automatically on
   first build.
2. Plug in a board and open this folder in VS Code.
3. Pick the `tx` or `rx` environment in the PlatformIO status bar, then
   build/upload/monitor — or from the CLI:
   ```
   pio run -e tx -t upload
   pio device monitor -e tx
   ```
4. Repeat for the second board with the other environment (`rx`/`tx`).

If you have **both boards plugged in at once**, PlatformIO can't
auto-detect which port is which — see [CLAUDE.md](CLAUDE.md#local-port-overrides)
for how to set per-machine port overrides via `platformio.local.ini`.

Full setup instructions for macOS and Windows, adding/updating libraries,
and multi-machine/multi-developer notes are in **[CLAUDE.md](CLAUDE.md)**.

## Project layout

```
src/main_tx.cpp   transmitter firmware (env:tx)
src/main_rx.cpp   receiver firmware (env:rx)
lib/              private/local libraries (none yet)
include/          shared headers (none yet)
platformio.ini    board, framework, and library config
```
