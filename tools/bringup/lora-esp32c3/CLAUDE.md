# LoRa TX/RX Firmware

PlatformIO project for two Seeed XIAO ESP32C3 boards running a LoRa
point-to-point link (SX127x via the `sandeepmistry/LoRa` library, Arduino
framework). One board builds as the `tx` environment (transmitter,
[src/main_tx.cpp](src/main_tx.cpp)), the other as `rx`
(receiver, [src/main_rx.cpp](src/main_rx.cpp)). Both share the LoRa wiring
defined at the top of each file: `SS=20`, `RST=2`, `DIO0=3`, 915 MHz.

## Prerequisites

- **VS Code** (or Cursor/any VS Code fork)
- **pioarduino IDE extension** for VS Code — this repo's [.vscode/extensions.json](.vscode/extensions.json)
  recommends `pioarduino.pioarduino-ide` (a PlatformIO fork); do not install
  the standard PlatformIO IDE extension alongside it.
- Alternatively, the **PlatformIO Core CLI** (`pip install platformio` or
  `pipx install platformio`) works without VS Code — all commands below have
  a `pio` CLI equivalent.
- Two Seeed XIAO ESP32C3 boards connected via USB-C.

No manual toolchain install is needed — PlatformIO downloads the
`espressif32` platform, Arduino framework, and the `LoRa` library dependency
automatically on first build (per [platformio.ini](platformio.ini)).

## macOS setup

1. Install VS Code, then install the `pioarduino.pioarduino-ide` extension
   (Extensions panel will prompt you to install workspace recommendations —
   accept it).
2. Plug in a board. The XIAO ESP32C3 uses the ESP32-C3's native USB, so no
   driver install is required on modern macOS.
3. If you only have one board plugged in, skip ahead — PlatformIO
   auto-detects its port. If you have both `tx` and `rx` boards plugged in
   at once, find their ports with `ls /dev/cu.usbmodem*` and set them up
   per the "Local port overrides" section below.
4. Build/upload/monitor via the PlatformIO sidebar (pick the `tx` or `rx`
   environment at the bottom status bar), or via CLI:
   ```
   pio run -e tx -t upload
   pio device monitor -e tx
   ```


   pio run -e rx -t upload
   pio device monitor -e rx
## Windows setup

1. Install VS Code, then install the `pioarduino.pioarduino-ide` extension.
2. Plug in a board. If Windows doesn't enumerate a COM port for it, install
   the USB driver for the XIAO ESP32C3 from the
   [Seeed wiki](https://wiki.seeedstudio.com/XIAO_ESP32C3_Getting_Started/)
   (driver needed only on some Windows builds/board revisions).
3. If you only have one board plugged in, skip ahead — PlatformIO
   auto-detects its port. If both boards are plugged in at once, find their
   ports in Device Manager under "Ports (COM & LPT)" (e.g. `COM5`) and set
   them up per the "Local port overrides" section below.
4. Build/upload/monitor the same way as macOS, substituting your COM ports:
   ```
   pio run -e tx -t upload
   pio device monitor -e tx
   ```

   pio run -e rx -t upload
   pio device monitor -e rx

## Managing libraries

Libraries are declared in [platformio.ini](platformio.ini) under `lib_deps`
(currently `sandeepmistry/LoRa @ ^0.8.0` in the shared `[env]` section, so
both `tx` and `rx` get it). You don't need to manually download or install
anything — PlatformIO resolves and fetches everything listed in `lib_deps`
automatically the next time you build, into `.pio/libdeps/<env>/`.

**To add a new library:**
1. Find its PlatformIO registry name, e.g. search at
   https://registry.platformio.org or run:
   ```
   pio pkg search "<keyword>"
   ```
2. Add a line to `lib_deps` in `platformio.ini`:
   - Add to the shared `[env]` section if both `tx` and `rx` need it.
   - Add to `[env:tx]` or `[env:rx]` if only one side needs it.
   - Pin a version to keep builds reproducible, e.g. `author/LibName @ ^1.2.3`.
3. Build (`pio run` or the VS Code build button) — PlatformIO installs the
   new dependency automatically. No separate "install" step is required,
   and nothing needs to be committed besides the `platformio.ini` change
   (`.pio/` stays gitignored).

**Useful CLI commands** (equivalents exist in the PlatformIO/pioarduino
sidebar under "Libraries"):
```
pio pkg list              # show installed libraries per environment
pio pkg update            # update libraries to the latest allowed version
pio pkg install -l "author/LibName@^1.0.0"   # install without editing lib_deps by hand
```

**Private/local libraries**: if you write your own reusable code instead of
pulling from the registry, put it in `lib/<YourLibName>/` (see
[lib/README](lib/README)) — PlatformIO's dependency finder picks it up
automatically without any `lib_deps` entry.

## Local port overrides

[platformio.ini](platformio.ini) intentionally does **not** hardcode
`upload_port` / `monitor_port` — those are machine- and OS-specific, and
committing them just breaks the next person who clones the repo. With a
single board plugged in, PlatformIO auto-detects the port and you don't
need to do anything.

If you have both `tx` and `rx` boards plugged in at the same time (so
auto-detection is ambiguous):

1. Copy the template: `cp platformio.local.ini.example platformio.local.ini`
2. Edit it with your actual ports (macOS `/dev/cu.usbmodem*`, Windows
   `COM<n>`).
3. `platformio.local.ini` is gitignored and merged in automatically via
   `extra_configs` in `platformio.ini` — it never needs to be committed or
   touched again after the first setup on a given machine.

You can also skip the file entirely and pass the port per-command:
`pio run -e tx -t upload --upload-port <port>`.

## Notes for multi-machine / multi-developer use

- `.vscode/c_cpp_properties.json` and `.vscode/launch.json` are
  auto-generated by PlatformIO per machine (they bake in absolute local
  paths) and are gitignored — don't commit them. They regenerate the first
  time you open/build the project in VS Code with the PlatformIO extension.
- If both boards are plugged in at once, build one environment at a time
  (`-e tx` or `-e rx`) so PlatformIO targets the right port.
- `.pio/` is build output and gitignored — safe to delete if a build gets
  into a bad state (`pio run -t clean` or `rm -rf .pio`).
