# Astronaut Biometric Sensor Array (repo root)

This is a monorepo with independent subsystems, each with its own
toolchain and its own detailed setup docs. This file only covers
repo-wide conventions; for anything specific to a subsystem, go to its
own README/CLAUDE.md:

- **[firmware/lora-testing](firmware/lora-testing)** — PlatformIO firmware
  for the LoRa TX/RX boards. See
  [firmware/lora-testing/CLAUDE.md](firmware/lora-testing/CLAUDE.md) for
  macOS/Windows setup, library management, and port configuration.
- **[firmware/main-app](firmware/main-app)** — firmware for the real
  sensor-node board. See
  [firmware/main-app/README.md](firmware/main-app/README.md) for the
  current TODO list.
- **[machine-learning](machine-learning)** — ML on captured
  biometric/sensor data. See
  [machine-learning/README.md](machine-learning/README.md).
- **[web-ui](web-ui)** — monitoring/visualization web app. See
  [web-ui/README.md](web-ui/README.md).

## Repo-wide conventions

- Each subsystem folder is self-contained: its own dependency manifest,
  its own `.gitignore` for tool-specific artifacts (build output,
  `node_modules`, virtualenvs, etc.), and its own README for setup.
  The root [.gitignore](.gitignore) only holds cross-cutting OS/editor
  junk (`.DS_Store`, etc.) — add subsystem-specific ignores inside that
  subsystem's own `.gitignore`, not the root one.
- Don't add cross-subsystem dependencies unless there's a real shared
  need (e.g. a common data format between firmware and machine-learning)
  — keep each subsystem buildable/runnable independently.
- When adding a new subsystem, give it its own top-level folder with a
  README, and add a row to the table in the root [README.md](README.md).
