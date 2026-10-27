# Astronaut Biometric Sensor Array (repo root)

The repo is organized by device, not by team. See the layout table in the
root [README.md](README.md). Each project folder has its own toolchain and
README; this file only covers repo-wide conventions.

## Repo-wide conventions

- `devices/<device>/` holds what ships: `firmware/`, `tests/`, `README.md`.
  Use `firmware/<mcu>/` only if one device has several MCU targets.
- `tools/` holds what engineers use to build and test, including the test
  base station, testing web UIs and all bring-up/prototype firmware
  (`tools/bringup/`). Prototypes graduate into `devices/` when they become
  the real firmware.
- Unit tests go in `devices/<device>/tests/`; multi-device tests go in the
  top-level `tests/`.
- `common/` is only for code used by two or more devices. Packet/protocol
  definitions live in exactly one place: `common/protocol/` in code, and
  [docs/protocol.md](docs/protocol.md) for the format.
- Team ownership lives in [.github/CODEOWNERS](.github/CODEOWNERS), never in
  folder names.
- Each project folder is self-contained: its own dependency manifest, its
  own `.gitignore` for tool artifacts (build output, `node_modules`,
  virtualenvs), and its own README. The root [.gitignore](.gitignore) only
  holds OS/editor junk.
- Don't add Claude as a co-author or contributor anywhere: no
  `Co-Authored-By` lines or Claude attribution in commits or PRs.
