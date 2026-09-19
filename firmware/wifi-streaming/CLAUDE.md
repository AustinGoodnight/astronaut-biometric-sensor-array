# wifi-streaming (MAX30102 + WiFi bring-up)

PlatformIO project for the same MAX30102 heart-rate/pulse-ox bring-up as
[firmware/hr-testing](../hr-testing), on the same Seeed XIAO ESP32C6, but
streaming over WiFi (TCP) instead of BLE. Sibling to hr-testing and
[firmware/lora-testing](../lora-testing) - see lora-testing's CLAUDE.md for
the fuller macOS/Windows setup writeup this one mirrors; only the parts that
differ are repeated here.

## Why WiFi instead of BLE

hr-testing's BLE version (`main_filtered.cpp`) worked, verified end-to-end
with sustained ~50 samples/s and zero packet loss - but only when macOS
happened to grant a fast BLE connection interval. On macOS, a peripheral can
*request* a connection interval (this firmware's BLE counterpart calls
`NimBLEServer::updateConnParams` and advertises `setPreferredParams`), but
CoreBluetooth alone decides whether to actually honor it, and that decision
isn't visible or controllable from either the firmware or the Python client.
Confirmed empirically: identical firmware, reconnecting repeatedly, sometimes
got a 30ms interval (full raw-rate streaming) and sometimes settled near the
spec's slow end (~4s), where a stream that notifies far more often than once
per interval gets almost nothing through. After enough reconnects during
testing, it got stuck consistently slow, and neither a Bluetooth toggle nor
an app-level retry loop reliably cleared it - a deeper, OS-level state
problem rather than anything fixable in firmware or the client script.

WiFi TCP has no equivalent per-connection black box: once connected,
throughput is deterministic and not subject to an invisible OS policy
decision. That's the entire reason this project exists alongside
hr-testing rather than just patching hr-testing further - it's a genuinely
different transport with different failure modes, not a drop-in swap.

## Prerequisites

- **VS Code** (or Cursor/any VS Code fork)
- **pioarduino IDE extension** for VS Code — this repo's
  [.vscode/extensions.json](.vscode/extensions.json) recommends
  `pioarduino.pioarduino-ide` (a PlatformIO fork); do not install the
  standard PlatformIO IDE extension alongside it.
- Alternatively, the **PlatformIO Core CLI** (`pip install platformio` or
  `pipx install platformio`) works without VS Code.
- A Seeed XIAO ESP32C6 and a MAX30102 breakout wired per
  [README.md](README.md#wiring).
- A WiFi network the board and your computer can both reach (same LAN/AP -
  this streams over plain TCP with no internet-facing component).

No manual toolchain install is needed — PlatformIO downloads the
`espressif32` platform, Arduino framework, and the SparkFun MAX3010x library
automatically on first build (per [platformio.ini](platformio.ini)).

## WiFi credentials

Copy [include/wifi_secrets.h.example](include/wifi_secrets.h.example) to
`include/wifi_secrets.h` (gitignored, per-network rather than per-machine
like `platformio.local.ini`) and fill in `WIFI_SSID`/`WIFI_PASSWORD`. The
build fails with a clear `#error` if this file is missing, rather than
compiling with placeholder credentials that would silently never connect.

## Build/upload/monitor

```
pio run -e xiao_c6 -t upload
pio device monitor -e xiao_c6
```

Or use the PlatformIO sidebar with the `xiao_c6` environment selected in the
VS Code status bar. Watch the serial output after boot for the board's IP
address - the Python clients need it.

## Streaming clients

Two independent plain-TCP servers run on the board, mirroring hr-testing's
two BLE characteristics/scripts:

| Port | Purpose | Client |
|------|---------|--------|
| 3333 | Human-readable status/beat text log | `wifi_monitor.py <board-ip>` |
| 3334 | Binary raw IR/Red sample stream (batched, see `RawSample` in `src/main.cpp`) | `wifi_raw_stream.py <board-ip> [--csv out.csv]` |

Both are pure standard-library Python (`socket`) - no `pip install` needed,
unlike hr-testing's BLE scripts which depend on `bleak`. Each server accepts
one client at a time; connecting a second client drops the first.

## Managing libraries

Libraries are declared in [platformio.ini](platformio.ini) under `lib_deps`
(currently `sparkfun/SparkFun MAX3010x Pulse and Proximity Sensor Library @
^1.1.2`; WiFi/TCP support comes from the Arduino-ESP32 framework's bundled
`WiFi.h`, no separate dependency). PlatformIO resolves and fetches
everything listed there automatically on the next build, into
`.pio/libdeps/xiao_c6/`.

**To add a new library:**
1. Find its PlatformIO registry name, e.g.
   `pio pkg search "<keyword>"` or https://registry.platformio.org.
2. Add a line to `lib_deps` in `platformio.ini`, pinning a version
   (`author/LibName @ ^1.2.3`) to keep builds reproducible.
3. Build — PlatformIO installs it automatically. Nothing needs to be
   committed besides the `platformio.ini` change (`.pio/` stays gitignored).

**Private/local libraries**: put your own reusable code in
`lib/<YourLibName>/` (see [lib/README](lib/README)) — PlatformIO's
dependency finder picks it up automatically.

## Local port overrides

[platformio.ini](platformio.ini) intentionally does **not** hardcode
`upload_port` / `monitor_port` — those are machine- and OS-specific. With a
single board plugged in, PlatformIO auto-detects the port automatically.

If auto-detection is ever ambiguous (e.g. another board also plugged in):

1. Copy the template: `cp platformio.local.ini.example platformio.local.ini`
2. Edit it with your actual port (macOS `/dev/cu.usbmodem*`, Windows
   `COM<n>`).
3. `platformio.local.ini` is gitignored and merged in automatically via
   `extra_configs` in `platformio.ini` — no need to commit or touch it again.

You can also skip the file entirely and pass the port per-command:
`pio run -e xiao_c6 -t upload --upload-port <port>`.

## Notes

- `.vscode/c_cpp_properties.json` and `.vscode/launch.json` are
  auto-generated by PlatformIO per machine and are gitignored — don't
  commit them.
- `.pio/` is build output and gitignored — safe to delete if a build gets
  into a bad state (`pio run -t clean` or `rm -rf .pio`).
- `*.csv` is gitignored — captured sample data from `--csv` shouldn't be
  committed.
