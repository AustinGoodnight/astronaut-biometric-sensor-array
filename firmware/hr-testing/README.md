# MAX30102 Heart-Rate Sensor Bring-Up

A minimal test harness for a [MAX30102](https://www.maximintegrated.com/en/products/sensors/MAX30102.html)
pulse-oximetry/heart-rate sensor wired to a
[Seeed XIAO ESP32C6](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/)
over I2C, built with [PlatformIO](https://platformio.org) and the Arduino
framework. It initializes the sensor via the SparkFun MAX3010x library and
streams raw IR/Red ADC samples over serial so you can confirm wiring and
sensor communication before building the real HR/SpO2 pipeline.

| | |
|---|---|
| **Board**  | Seeed XIAO ESP32C6 |
| **Sensor** | MAX30102 (via [`sparkfun/SparkFun MAX3010x Pulse and Proximity Sensor Library`](https://github.com/sparkfun/SparkFun_MAX3010x_Sensor_Library)) |
| **Bus**    | I2C |
| **Code**   | [src/main.cpp](src/main.cpp) |

## Wiring

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
2. Wire the MAX30102 breakout per the table above and plug in the board.
3. Build/upload/monitor from the PlatformIO sidebar (`xiao_c6` environment),
   or via CLI:
   ```
   pio run -e xiao_c6 -t upload
   pio device monitor -e xiao_c6
   ```
4. Place a finger over the sensor — you should see `ir,red` sample pairs
   stream in; without a finger present it prints `No finger detected`.

If PlatformIO can't auto-detect the port, see
[CLAUDE.md](CLAUDE.md#local-port-overrides) for setting a per-machine
override via `platformio.local.ini`.

## Project layout

```
src/main.cpp      sensor bring-up + raw IR/Red streaming (env:xiao_c6)
lib/              private/local libraries (none yet)
include/          shared headers (none yet)
platformio.ini    board, framework, and library config
```
