# MAX30102 Heart-Rate Sensor + WiFi Streaming

Same [MAX30102](https://www.maximintegrated.com/en/products/sensors/MAX30102.html)
bring-up and beat-detection pipeline as
[firmware/hr-testing](../hr-testing), on the same
[Seeed XIAO ESP32C6](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/)
board, but streamed wirelessly over WiFi (TCP) instead of BLE.

hr-testing's BLE version worked, but macOS's CoreBluetooth only lets a
peripheral *request* a fast connection interval - the OS decides whether to
grant it, with no visibility into why, and it sometimes silently settled on
a slow one, starving the raw sample stream. WiFi has no equivalent
per-connection black box, so this project trades "works on a low-power
wearable" for "throughput just works, deterministically" - useful for
bring-up/data-capture even if the final deployment target ends up BLE-based.

| | |
|---|---|
| **Board**  | Seeed XIAO ESP32C6 |
| **Sensor** | MAX30102 (via [`sparkfun/SparkFun MAX3010x Pulse and Proximity Sensor Library`](https://github.com/sparkfun/SparkFun_MAX3010x_Sensor_Library)) |
| **Bus**    | I2C |
| **Transport** | WiFi, two plain TCP servers on the board (see below) |
| **Code**   | [src/main.cpp](src/main.cpp) |

## Wiring

Same as hr-testing:

| Signal | XIAO ESP32C6 pin |
|--------|-------------------|
| SDA    | GPIO22 (D4) |
| SCL    | GPIO23 (D5) |
| VIN    | 3V3 |
| GND    | GND |

## Quick start

1. Install [VS Code](https://code.visualstudio.com/) and the **pioarduino**
   extension (`pioarduino.pioarduino-ide`) — see
   [.vscode/extensions.json](.vscode/extensions.json). No separate
   PlatformIO toolchain install is needed; it's fetched automatically on
   first build.
2. Copy [include/wifi_secrets.h.example](include/wifi_secrets.h.example) to
   `include/wifi_secrets.h` (gitignored) and fill in your WiFi network's
   SSID/password.
3. Wire the MAX30102 breakout per the table above and plug in the board.
4. Build/upload/monitor from the PlatformIO sidebar (`xiao_c6` environment),
   or via CLI:
   ```
   pio run -e xiao_c6 -t upload
   pio device monitor -e xiao_c6
   ```
5. Watch the serial monitor for the board's IP address once it joins WiFi.
6. From your computer (same network), run either client:
   ```
   python3 wifi_monitor.py <board-ip>              # human-readable log
   python3 wifi_raw_stream.py <board-ip>            # raw IR/Red sample stream
   python3 wifi_raw_stream.py <board-ip> --csv out.csv
   ```
   Both use only the Python standard library (`socket`) - no packages to
   install, unlike the BLE scripts in hr-testing which needed `bleak`.

If PlatformIO can't auto-detect the upload port, see
[CLAUDE.md](CLAUDE.md#local-port-overrides) for setting a per-machine
override via `platformio.local.ini`.

## Project layout

```
src/main.cpp              sensor bring-up + beat detection + WiFi TCP streaming
include/wifi_secrets.h.example   template for WiFi credentials (copy, don't edit in place)
wifi_monitor.py            text log client (port 3333)
wifi_raw_stream.py         binary raw-sample client (port 3334)
lib/                        private/local libraries (none yet)
include/                    shared headers
platformio.ini              board, framework, and library config
```
