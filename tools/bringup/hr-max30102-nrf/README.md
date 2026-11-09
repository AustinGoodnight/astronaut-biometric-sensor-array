# MAX30102 Heart Rate / SpO₂ Bring-Up (XIAO nRF52840)

Standalone Arduino/PlatformIO bring-up for the MAX30102 on a Seeed XIAO
nRF52840. It preserves the red/IR acquisition window and Maxim's
heart-rate/SpO₂ algorithm from the ESP32-C6 version, and prints results over
USB serial. BLE transport is not included.

| | |
|---|---|
| **Board** | Seeed XIAO nRF52840 |
| **Sensor** | MAX30102, via the SparkFun MAX3010x library |
| **Bus** | I²C, 400 kHz |
| **Code** | [src/main.cpp](src/main.cpp) |

## Wiring

Use the XIAO's default I²C pins:

| Signal | XIAO nRF52840 pin |
|---|---|
| SDA | D4 |
| SCL | D5 |
| VIN | 3V3 |
| GND | GND |

Connect the sensor to 3.3 V and ensure its I²C pull-ups are suitable for 3.3 V.

## Build, upload, and monitor

Install PlatformIO, connect the board, then run:

```sh
pio run -e xiao_nrf52840
pio run -e xiao_nrf52840 -t upload
pio device monitor -e xiao_nrf52840
```

If multiple boards are connected or the port is not detected, copy
`platformio.local.ini.example` to `platformio.local.ini` and set the correct
port. The local file is git-ignored.

The firmware acquires 100 samples at a nominal 25 samples/second. After its
initial four-second window, it advances the window by 25 samples and updates
the HR and SpO₂ estimates once per second. Validity flags come from the Maxim
algorithm; readings should not be treated as medical measurements.
