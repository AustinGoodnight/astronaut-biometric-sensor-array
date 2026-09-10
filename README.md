# Astronaut Biometric Sensor Array

Monorepo for the capstone project: a LoRa-linked biometric sensor array,
plus the software that trains on and displays what it captures.

| Subsystem | What it is | Docs |
|---|---|---|
| [firmware/lora-testing](firmware/lora-testing) | PlatformIO firmware for the two Seeed XIAO ESP32C3 LoRa TX/RX boards | [README](firmware/lora-testing/README.md) · [CLAUDE.md](firmware/lora-testing/CLAUDE.md) |
| [machine-learning](machine-learning) | ML models on the captured biometric/sensor data | [README](machine-learning/README.md) |
| [web-ui](web-ui) | Web UI for monitoring/visualizing the link | [README](web-ui/README.md) |

Each subsystem has its own README with setup instructions specific to its
toolchain — there's no single shared build for the whole repo. Start with
the subsystem you're working on.

See [CLAUDE.md](CLAUDE.md) for repo-wide conventions.
