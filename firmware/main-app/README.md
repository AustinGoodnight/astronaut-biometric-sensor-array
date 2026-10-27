# Wrist Module Firmware (main-app)

Zephyr / nRF Connect SDK v3.4.0 firmware for the **wrist-worn sensor module**
(nRF52840). Every 5 s it builds a 32-byte summary record (HR, respiration,
skin and core temperature, GSR, activity, battery), logs it to local flash,
and sends it over BLE to the helmet board, which relays it over LoRa.

- Design, threads, record format: [ARCHITECTURE.md](ARCHITECTURE.md)
- Conventions for working in this code: [CLAUDE.md](CLAUDE.md)

**Status:** skeleton builds (debug, release, MCUboot variant) with no
warnings. The plumbing is real (threads, watchdog, health state machine,
packager, BLE service). Drivers and signal-processing algorithms are stubs
or placeholders. With fake sensors, the whole pipeline runs on a bare XIAO.

| Hardware | Role |
|---|---|
| Seeed XIAO nRF52840 on our breakout (`xiao_ble/nrf52840`) | Dev platform now. Runs the whole pipeline on fake sensors, then real sensors as they are wired up. |
| Custom wrist board (Raytac nRF52840 module) | Final hardware, in design. |

---

## Hardware requirements (what the firmware assumes)

These follow from the firmware design. If the board can't meet one, tell the
firmware side: most have a fallback, but it costs reliability or power.

### Power

- **MAX30101 core: 1.8 V LDO** (VDD 1.7–2.0 V), separate from the LED supply. ✅ planned
  - **Put the 1.8 V LDO behind bus A's load switch** (or switch the LDO's
    enable with the same GPIO). The firmware's last-resort fix for a hung
    PPG chip is to power-cycle bus A. If the 1.8 V stays up, that doesn't
    reset the MAX30101 core.
- **5 V boost for the PPG LEDs, with a GPIO enable.** Firmware turns it on
  only while PPG runs.
  - Pick a boost whose inductor current limit (or output capacitance) keeps the
    battery-side peak under ~25 mA during LED pulses. The whole board
    must stay under 30 mA peak.
- **One load switch per I2C bus, each with its own GPIO enable.** ✅ agreed
  - Bus A: MAX30101 (+ its 1.8 V LDO) + accelerometer
  - Bus B: skin temp (MAX30205/08) + fuel gauge (MAX17048)
  - A fault on one bus can then be fixed by power-cycling it without
    touching the other.
- **I2C pull-ups on the switched side of each load switch.** With the
  pull-ups on an always-on rail, the sensors get back-powered through
  SDA/SCL when their rail is off.
- **GSR front end and flex divider each have a GPIO power enable** (they
  are on only ~10 ms per second).
- **Push-button on/off controller with a power-hold GPIO** that the MCU can
  release to power itself off.
- **Raytac module:** confirm whether it includes the **DC/DC inductor** and
  the **32.768 kHz crystal**. The firmware config depends on both (enabling
  DC/DC without the inductor stops the chip from running).

### Signals

| Signal | Type | Notes |
|---|---|---|
| I2C bus A SDA/SCL | TWIM0 | PPG + accel |
| I2C bus B SDA/SCL | TWIM1 | skin temp + fuel gauge |
| PPG INT | GPIO in | MAX30101 FIFO-almost-full. **Required.** |
| Accel INT | GPIO in | Optional: firmware can drain the accel FIFO on the PPG interrupt instead. |
| Fuel gauge ALRT | GPIO in | MAX17048 low-battery alert. |
| Skin temp OS/INT | — | Not needed (polled). |
| GSR analog | SAADC AIN | |
| Flex analog | SAADC AIN | Only if respiration uses the flex sensor (open decision). |
| Rail A EN, Rail B EN | GPIO out | The per-bus load switches. |
| 5 V boost EN | GPIO out | |
| GSR EN, Flex EN | GPIO out | Can share one pin if needed. |
| Power hold | GPIO out | |
| Power button | GPIO in | Long hold = power off. |
| Status LEDs | GPIO out | ≥ 2 (link/mode, fault/battery); RGB is fine. |
| QSPI NOR flash | QSPI (6 pins) | Record log. ≥ 2 MB (≈ 4 days of records). |
| USB D+/D− + VBUS | USB | Charging, firmware update, debug console. |

### Debug and test

- **SWD header** (Tag-Connect is fine). A blank Raytac module can only be
  flashed the first time over SWD.
- **Test points** on both I2C buses (SDA, SCL), PPG INT, the 1.8 V rail, the
  5 V boost output, and each load-switch output. (The last charger board's
  MAX17048s couldn't be probed and the fault was never found.)

### Get an SWD probe now

The XIAO has no onboard debugger, and the Raytac board needs SWD for its
first flash. A **Raspberry Pi Debug Probe** (≈ $12) or **J-Link EDU Mini**
(≈ $60) also lets us install and test MCUboot (firmware-update rollback) on
the XIAO before the board exists.

---

## XIAO breakout pin map (proposed)

The XIAO has 11 header pins (D0–D10) plus two NFC pads on the back, which
can be used as GPIO. That's fewer than the full signal list above, so the
breakout drops or shares the least important ones:

| XIAO pin | nRF GPIO | Signal |
|---|---|---|
| D0 | P0.02 (AIN0) | GSR analog |
| D1 | P0.03 (AIN1) | Flex analog |
| D2 | P0.28 | PPG INT |
| D3 | P0.29 | Fuel gauge ALRT |
| D4 | P0.04 | Bus B SDA (skin temp, fuel gauge) |
| D5 | P0.05 | Bus B SCL |
| D6 | P1.11 | Bus A SDA (PPG, accel) |
| D7 | P1.12 | Bus A SCL |
| D8 | P1.13 | 5 V boost EN |
| D9 | P1.14 | GSR + flex front-end EN (shared) |
| D10 | P1.15 | Power button (to GND; internal pull-up) |
| NFC1 pad | P0.09 | Rail A EN (optional) |
| NFC2 pad | P0.10 | Rail B EN (optional) |
| onboard RGB | P0.26 / P0.30 / P0.06 | Status LEDs |

What the XIAO breakout leaves out, and how the firmware copes:

- **Accel INT:** not routed, so the firmware uses the "drain the accel on the
  PPG interrupt" mode. That's the fallback the custom board can also use.
- **Power hold:** the XIAO runs straight off USB or its battery pad, so power-off
  goes to nRF System OFF (the button wakes it).
- **Rail enables:** if the NFC pads aren't used, tie the rails on. Recovery
  then stops at bus recovery and re-init, without power-cycling.
- **Fuel gauge:** the XIAO has its own charger and a VBAT divider, not a
  MAX17048. Put a MAX17048 on the breakout to test it. Otherwise the firmware
  marks it NOT_PRESENT.

With `CONFIG_APP_FAKE_SENSORS=y`, none of this wiring is needed. A bare XIAO
runs the whole pipeline.

---

## Build and flash

**The repo path contains spaces, which Zephyr can't build from.** Build
through a space-free symlink. Toolchain setup matches MonoWatch (NCS v3.4.0
via Toolchain Manager):

```sh
HASH=$(ls /opt/nordic/ncs/toolchains | grep -v '\.json$' | head -1)
export PATH="/opt/nordic/ncs/toolchains/$HASH/bin:/opt/nordic/ncs/toolchains/$HASH/opt/zephyr-sdk/gnu/arm-zephyr-eabi/bin:$PATH"
export ZEPHYR_TOOLCHAIN_VARIANT="zephyr/gnu"
export ZEPHYR_SDK_INSTALL_DIR="/opt/nordic/ncs/toolchains/$HASH/opt/zephyr-sdk"
ln -sfn "<repo>/firmware/main-app" ~/.cache/wrist-fw   # once
cd /opt/nordic/ncs/v3.4.0
west build -b xiao_ble/nrf52840 -s ~/.cache/wrist-fw/app -d ~/.cache/wrist-fw/build/debug -- -DEXTRA_CONF_FILE=debug.conf
```

Swap `debug` for `release` to build the release config. Add `-p always`
after changing a Kconfig choice or an overlay.

| Config | Adds | Use |
|---|---|---|
| `prj.conf` | common base | always |
| `debug.conf` | logs + shell on USB, thread analyzer, asserts, **fake sensors** | development |
| `release.conf` | logging, console, shell and USB off; real drivers | battery runs |
| `mcuboot/*` | MCUboot + swap-with-revert (needs an SWD probe to install) | see [ARCHITECTURE.md §10.3](ARCHITECTURE.md) |

Other options (`-DCONFIG_...=y`): `APP_FAKE_SENSORS`, `APP_RESP_SRC_FLEX`
or `APP_RESP_SRC_PPG`, `APP_STORAGE_BACKEND_NOR` or `_SD`,
`APP_SKIN_TEMP_MAX30205` or `_MAX30208`, `APP_ACCEL_INT_ROUTED`. See
[app/Kconfig](app/Kconfig).

**Flash:** double-tap the XIAO's reset button, then drag
`build/debug/app/zephyr/zephyr.uf2` onto the drive that appears.

## Testing on the XIAO (fake sensors)

1. **Console:** `screen /dev/tty.usbmodem* 115200`. Logs start about 4 s
   after boot. Expect `boot #N`, `advertising as "Wrist"`, then every 5 s a
   line like
   `rec #12 valid=0x1ff health=0x0300 hr=60.0 skin=3312 gsr=800 batt=100 flags=0x00`
   (all 9 fields valid; health `0x0300` = flex NOT_PRESENT because respiration
   comes from PPG by default; everything else OK)
   plus `[stub] would write page …` every 8 records.
2. **BLE:** in nRF Connect for Mobile, connect to `Wrist`. If your phone
   doesn't negotiate a larger MTU automatically, request one (≥ 35). Enable
   notifications on characteristic `7a1e0002-…` to get one 32-byte record
   every 5 s. Read `7a1e0003-…` for status.
3. **Fault injection** (in the shell):
   - `fake fault skin_temp nack`: within a few seconds `wrist health` shows
     skin_temp DEGRADED, then FAILED after the recovery ladder (bus recovery,
     re-init, power-cycle of bus B). Validity bit 4 (0x010) clears, health
     becomes `0x0320` (slot 2 = FAILED), and records keep coming every 5 s.
     `fake fault skin_temp none` brings it back on the next 60 s retry.
   - `fake fault ppg flatline`: the HR bits (0x007), plus PPG-derived
     respiration (0x008) and core temp (0x020), clear because validation
     rejects the signal, while PPG health stays OK (the bus is fine; the
     data is bad).
   - `fake fault gsr nack 5`: a transient fault that recovers by itself.
   - `wrist stats` shows the counters; `kernel thread stacks` shows stack
     usage.
