# tools/bringup/zephyr-main-app — wrist module firmware

Zephyr / **nRF Connect SDK v3.4.0** firmware for the wrist-worn sensor module
(nRF52840). Dev hardware: Seeed XIAO nRF52840 on our breakout
(`xiao_ble/nrf52840`); the custom Raytac board comes later. **Read [ARCHITECTURE.md](ARCHITECTURE.md) before changing code**:
it defines the threads, data contracts, record format and the reasons behind
them. If a change contradicts it, update ARCHITECTURE.md in the same commit
(or ask first).

Priorities, in order: **reliability → low power → simplicity.**

## Build

Three gotchas, all handled by the recipe below:
- `west` isn't on PATH, and this repo isn't a west workspace. Build against
  the Toolchain Manager install, the same way as MonoWatch.
- **The repo path has spaces** ("Capstone Code"), which Zephyr's build
  system can't handle (`File not found: .../Capstone`). Build through a
  space-free symlink.
- Build *from* the NCS topdir (`cd /opt/nordic/ncs/v3.4.0`), or west says
  `unknown command "build"`.

```sh
HASH=$(ls /opt/nordic/ncs/toolchains | grep -v '\.json$' | head -1)
export PATH="/opt/nordic/ncs/toolchains/$HASH/bin:/opt/nordic/ncs/toolchains/$HASH/opt/zephyr-sdk/gnu/arm-zephyr-eabi/bin:$PATH"
export ZEPHYR_TOOLCHAIN_VARIANT="zephyr/gnu"
export ZEPHYR_SDK_INSTALL_DIR="/opt/nordic/ncs/toolchains/$HASH/opt/zephyr-sdk"
mkdir -p ~/.cache && ln -sfn "/Users/austingoodnight/Documents/Capstone Code/Astronaut Biometric Sensor Array/tools/bringup/zephyr-main-app" ~/.cache/wrist-fw
W=~/.cache/wrist-fw
cd /opt/nordic/ncs/v3.4.0

# debug (fake sensors, logs + shell on USB) and release
west build -b xiao_ble/nrf52840 -s $W/app -d $W/build/debug   -- -DEXTRA_CONF_FILE=debug.conf
west build -b xiao_ble/nrf52840 -s $W/app -d $W/build/release -- -DEXTRA_CONF_FILE=release.conf

# MCUboot variant (needs an SWD probe to flash; see ARCHITECTURE.md §10.3)
west build -b xiao_ble/nrf52840 -s $W/app -d $W/build/mcuboot -- \
	-DSB_CONF_FILE=mcuboot/sysbuild.conf \
	"-DEXTRA_CONF_FILE=debug.conf;mcuboot/app.conf" \
	-DEXTRA_DTC_OVERLAY_FILE=mcuboot/partitions.overlay
```

- A change counts as done only when **debug and release** build with **zero
  warnings** and the MCUboot variant still builds. Check the full log for
  `warning:`, including devicetree and Kconfig warnings. Expected and allowed:
  debug prints a CMake notice that `__ASSERT()` is enabled, and the MCUboot
  variant prints three bootloader production TODOs (default signing key,
  flash patch, CC310), listed in `app/sysbuild/mcuboot.conf`.
- Use a pristine build (`-p always`) after changing Kconfig choices, an
  overlay, or a sysbuild file. `build/` is git-ignored.
- Swappable options to spot-check when touching their code (pass them as
  `-DCONFIG_...=y`): `APP_FAKE_SENSORS=n`, `APP_RESP_SRC_FLEX`,
  `APP_STORAGE_BACKEND_SD`, `APP_SKIN_TEMP_MAX30208`, `APP_ACCEL_INT_ROUTED`.
- Flashing: **UF2 drag-and-drop only, and the user does it.** Never try to
  flash; build, then tell the user which `zephyr.uf2` to drag
  (`build/<cfg>/app/zephyr/zephyr.uf2`). Debug logs and the shell are on the
  XIAO's USB CDC ACM port (no RTT).

## Hard rules (from ARCHITECTURE.md; don't break them silently)

- **`acq` is the only thread that touches I2C or the SAADC.** Other threads
  request work by posting an event bit (see `include/app/events.h`).
- **ISRs only post `k_event` bits or give semaphores.** No I2C, logging or
  allocation in ISR or `k_timer` expiry context.
- **No heap.** No `k_malloc`/`malloc`; `CONFIG_HEAP_MEM_POOL_SIZE=0`. Use
  static buffers, `K_MEM_SLAB`, `K_MSGQ`, zbus.
- **Never block on downstream.** Producers use `K_NO_WAIT`; when a queue or slab
  is full, drop and increment a counter in `counters.h`.
- **The packager never waits** for sensors or results; a missing or old value
  just clears its validity bit.
- **Invalid values are flagged, never replaced** with a held or last-good
  value.
- **Every thread feeds its task-watchdog channel**, and every blocking wait has
  a timeout shorter than that channel's timeout. No `K_FOREVER` in thread loops.
- **Per-module state lives in one file-static context struct.** Threads keep
  no algorithm state. Only `proc` calls processing modules, so contexts need
  no locks.
- **No polling loops** and no `k_sleep` to wait for sensor data. Use
  interrupts and timers.
- Code references **devicetree labels and `zephyr,user` properties**, never
  pin numbers.

## Conventions

- C11, Zephyr coding style (tabs, 100 columns, `snake_case`), one
  `LOG_MODULE_REGISTER(<module>, CONFIG_APP_LOG_LEVEL)` per `.c` file.
- Public headers live in `app/include/app/…`. Every public function's doc
  comment states: **purpose, which thread calls it, whether it can block,
  return codes** (0 / negative errno). Module-private items are `static`.
- Each module exposes `<module>_init()`, called from `main.c` in the
  documented order. Threads are `K_THREAD_DEFINE`d with `K_TICKS_FOREVER`
  and started by `main` after all inits.
- Stubs return `-ENOSYS` (or a documented placeholder) and carry a
  `/* TODO: … */` describing the intended implementation.
- **Fixed-point** for everything published or transmitted. Scale factors
  are defined only in `include/app/units.h`. Floats are allowed only inside
  `src/processing/` and `src/sim/`.
- The record layout lives in `include/app/record.h`, guarded by `BUILD_ASSERT`
  size and offset checks. **Any layout change bumps `RECORD_FORMAT_VERSION`**
  and updates the ARCHITECTURE.md table and the base-station decoder.
- `include/app/ble_proto.h` must stay includable by the helmet firmware: fixed-width
  types, packed structs, `#define`s, and no Zephyr headers.
- New Kconfig options go in `app/Kconfig` with an `APP_` prefix and a help
  text. Debug-only things go in `debug.conf`, never in `prj.conf`.
- New hardware parts go behind the existing `include/app/drv/*.h`
  interface, with a matching fake in `src/sim/fake_sensors.c`, so the
  pipeline still runs with `CONFIG_APP_FAKE_SENSORS=y`.

## Git

- **Never add Claude as an author, co-author or contributor**: no
  `Co-Authored-By`, no "Generated with" lines, and no attribution in
  commits, trailers or PRs.
- Subsystem-specific ignores go in `tools/bringup/zephyr-main-app/.gitignore`, not
  the repo root.

## Open decisions

See ARCHITECTURE.md §13. Don't silently decide any of them in code. Keep
them behind the Kconfig choice or interface listed there.
