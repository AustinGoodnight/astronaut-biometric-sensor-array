# LoRa TX/RX Link — XIAO ESP32C6 + Wio-SX1262

A port of [../lora-testing](../lora-testing) to [Seeed XIAO ESP32C6](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/)
boards with the **Wio-SX1262 for XIAO** LoRa module. Same behavior: the
transmitter sends a counter/dummy sensor packet every 2 seconds at 915 MHz,
and the receiver prints each packet plus its RSSI/SNR to serial.

| | |
|---|---|
| **Board**    | Seeed XIAO ESP32C6 |
| **Radio**    | Wio-SX1262 (SX1262, via [`jgromes/RadioLib`](https://github.com/jgromes/RadioLib)) |
| **Frequency**| 915 MHz |
| **TX code**  | [src/main_tx.cpp](src/main_tx.cpp) |
| **RX code**  | [src/main_rx.cpp](src/main_rx.cpp) |

## Wiring

The Wio-SX1262 for XIAO plugs straight onto the XIAO headers. Pins are
defined at the top of each `main_*.cpp`:

| Signal | XIAO pin |
|--------|----------|
| NSS (CS) | D4 |
| DIO1 (IRQ) | D1 |
| NRST     | D2 |
| BUSY     | D3 |
| RF_SW    | D5 |
| SPI (SCK/MISO/MOSI) | D8 / D9 / D10 (default hardware SPI) |

The module's TCXO is powered from DIO3 at 1.8 V and DIO2 drives the RF
switch — both configured in `setup()`.

Note: D4/D5 are also the XIAO's default I2C pins, so an I2C sensor can't
share those pins with this module.

## Quick start

Identical to [../lora-testing](../lora-testing/README.md): open this folder
in VS Code with the pioarduino extension, pick the `tx` or `rx`
environment, and build/upload/monitor — or from the CLI:
```
pio run -e tx -t upload
pio device monitor -e tx
```
Repeat for the second board with `rx`. With both boards plugged in at once,
copy `platformio.local.ini.example` to `platformio.local.ini` and set
the ports (see [../lora-testing/CLAUDE.md](../lora-testing/CLAUDE.md#local-port-overrides)).

## Live config

The firmware speaks the same `SET sf=..,bw=..,pwr=..` / `GET` serial
protocol as the C3 boards, so [../lora-testing/config-ui](../lora-testing/config-ui)
works with these boards unchanged. The SX1262 accepts TX power from -9 to
22 dBm (the UI slider covers 2–20).

## Differences from lora-testing

- RadioLib instead of `sandeepmistry/LoRa` (that library only supports SX127x).
- Packets have CRC enabled (RadioLib default), and the RX side reports SNR too.
- Changing settings over serial puts the radio in standby first (the SX1262
  requires it), so a packet on air when you hit Apply gets dropped.
- These boards are not set up to talk to the SX127x boards in lora-testing.
