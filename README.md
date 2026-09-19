# Astronaut Biometric Sensor Array

Monorepo for the capstone project: a LoRa-linked biometric sensor array,
plus the software that trains on and displays what it captures.

| Subsystem | What it is | Docs |
|---|---|---|
| [firmware/lora-testing](firmware/lora-testing) | PlatformIO firmware for the two Seeed XIAO ESP32C3 LoRa TX/RX boards | [README](firmware/lora-testing/README.md) · [CLAUDE.md](firmware/lora-testing/CLAUDE.md) |
| [firmware/hr-testing](firmware/hr-testing) | PlatformIO firmware bringing up a MAX30102 heart-rate sensor on a Seeed XIAO ESP32C6, streamed over BLE | [README](firmware/hr-testing/README.md) · [CLAUDE.md](firmware/hr-testing/CLAUDE.md) |
| [firmware/wifi-streaming](firmware/wifi-streaming) | Same MAX30102 bring-up as hr-testing, streamed over WiFi (TCP) instead of BLE | [README](firmware/wifi-streaming/README.md) · [CLAUDE.md](firmware/wifi-streaming/CLAUDE.md) |
| [firmware/main-app](firmware/main-app) | Firmware for the real sensor-node board (per-second sensor read + LoRa transmit) | [README](firmware/main-app/README.md) |
| [machine-learning](machine-learning) | ML models on the captured biometric/sensor data | [README](machine-learning/README.md) |
| [web-ui](web-ui) | Web UI for monitoring/visualizing the link | [README](web-ui/README.md) |

Each subsystem has its own README with setup instructions specific to its
toolchain — there's no single shared build for the whole repo. Start with
the subsystem you're working on.

## Setup

### firmware/lora-testing

PlatformIO firmware for two Seeed XIAO ESP32C3 boards (one TX, one RX)
talking over a 915 MHz LoRa radio.

1. Install [VS Code](https://code.visualstudio.com/) and the
   **pioarduino** extension (`pioarduino.pioarduino-ide`). No separate
   PlatformIO toolchain install is needed — it's fetched automatically on
   first build.
2. Plug in a board and open [firmware/lora-testing](firmware/lora-testing)
   in VS Code.
3. Pick the `tx` or `rx` environment in the PlatformIO status bar, then
   build/upload/monitor — or from the CLI:
   ```
   pio run -e tx -t upload
   pio device monitor -e tx
   ```
4. Repeat for the second board with the other environment.

If both boards are plugged in at once, see
[firmware/lora-testing/CLAUDE.md](firmware/lora-testing/CLAUDE.md#local-port-overrides)
for per-machine port overrides. Full macOS/Windows setup and library
management notes are in that same CLAUDE.md.

### machine-learning

Machine Learning using Pytorch and Exutorch 
** Not yet configured **

### web-ui

Web UI with Node, Tailwind, and likely Astro
** Not yet configured **
