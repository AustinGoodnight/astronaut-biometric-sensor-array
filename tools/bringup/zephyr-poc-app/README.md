# Wrist POC (Zephyr, XIAO nRF52840)

Proof-of-concept wrist firmware on Nordic's nRF Connect SDK (Zephyr). It
reads heart rate (MAX30101 on I2C1) and skin resistance (ADC on AIN0/D0),
then sends a 15-byte packet over BLE (Nordic UART Service, advertised as
`Wrist-POC`) to the overview dashboard.

## Which board do I have?

| Board | Build target | What you get |
|---|---|---|
| Seeed XIAO nRF52840 (plain) | `xiao_ble/nrf52840` | Everything except motion removal |
| Seeed XIAO nRF52840 **Sense** | `xiao_ble/nrf52840/sense` | Same, plus the on-board IMU, so the spectral heart rate can remove motion |

If you're not sure, use the plain target. It runs on both boards; on a Sense
it just ignores the IMU.

## Building

Run from this folder, with the nRF Connect SDK v3.4.0 toolchain active.

```sh
# Plain XIAO
west build -b xiao_ble/nrf52840 -d build/xiao

# XIAO Sense
west build -b xiao_ble/nrf52840/sense -d build/sense

# Flash whichever matches the board on your desk
west flash -d build/xiao     # or build/sense
```

Keeping a build folder per board lets you switch between them without a full
rebuild. If you reuse one folder for both, add `-p` when you change boards.

In the nRF Connect VS Code extension: **Add build configuration**, pick the
board target from the table above, and leave everything else at its default.
The extension finds the right overlay and `.conf` files on its own.

At boot the serial log says which build is running:

```
Board: XIAO build, motion removal OFF (no IMU)
Board: XIAO Sense build, motion removal ON
```

## Where things live

| File | What it's for |
|---|---|
| `prj.conf` | Kconfig for every board |
| `xiao_ble_shared.dtsi` | **Pins and sensors for every board. Edit this one.** |
| `xiao_ble_nrf52840.overlay` | Plain board: only includes the shared file |
| `xiao_ble_nrf52840_sense.overlay` | Sense: includes the shared file; Sense-only devicetree goes here |
| `boards/xiao_ble_nrf52840_sense.conf` | Sense-only Kconfig (IMU rate and range). Never put IMU options in `prj.conf`: they don't exist on the plain board and break its build. |
| `src/main.c` | Startup, sensor polling, BLE packet layout |
| `src/max/hr_main.c` | Heart-rate glue: reads the MAX30101, runs both algorithms, status line, published HR |
| `src/max/spectral_hr.c` | Spectral heart rate (FFT, motion notches, tracker); published by default. The FFT is CMSIS-DSP's `arm_rfft_fast_f32` (ships with NCS, turned on in `prj.conf`) |
| `src/max/classic_hr.c` | Classic heart rate (autocorrelation, still wrist only) |
| `src/max/bandpass.h` | The 0.7-3.5 Hz filter every path shares |
| `src/max/bench.c` | USB bench protocol for the testing pages (commands and `raw,`/`mc,`/`status:` lines) |
| `src/persp/` | Skin resistance (ADC) |
| `src/ble/` | BLE advertising and NUS |
| `src/motion/` | IMU access for motion removal (Sense only, see below) |

## Motion removal (Sense only)

The Sense has an LSM6DS3TR-C IMU on its own internal I2C bus (`i2c0`), so it
never shares a bus with the MAX30101 on `i2c1`. `src/motion/motion.c` is
compiled only when the IMU driver is enabled (`CONFIG_LSM6DSL`, which turns
on by itself for the Sense target).

Call it from anywhere without `#if`:

```c
#include "motion/motion.h"

float accel[3];
if (motion_available() && motion_read_accel(accel) == 0) {
    /* accel[0..2] in m/s^2 */
}
```

The spectral heart rate uses it to find the arm's motion rhythms and notch them
out of the PPG spectrum (see `src/max/spectral_hr.c`). On the plain board
`motion_available()` is always false and the code compiles to nothing, so the
spectral tracker just skips motion removal.
