# battery-sense (dual-charger fuel gauges)

Bring-up/test firmware for the battery sensing on the DualUSBCharging board:
two MAX17048 fuel gauges (U4 on BAT1, U9 on BAT2) behind a TS5A23157 analog
switch (U5), plus the LTC4412 power-path status line (U6). It reads both
gauges through the switch and streams CSV over serial.

It builds for two targets from the same source:

| Env | MCU | Framework | Pins defined in |
|---|---|---|---|
| `nrf52820` | nRF52820 (nRF52833 DK in nRF52820 emulation) | Zephyr | [zephyr/boards/nrf52833dk_nrf52820.overlay](zephyr/boards/nrf52833dk_nrf52820.overlay) |
| `xiao_c6` | Seeed XIAO ESP32C6 | Arduino | top of [src/hal_arduino.cpp](src/hal_arduino.cpp) |

All test logic is in [src/battery_test.cpp](src/battery_test.cpp). The two
targets differ only in a small HAL ([src/hal.h](src/hal.h)): I2C read/write,
two GPIOs, delay, and print. To port to a different MCU, write one more
`hal_*.cpp`.

## How the board works (what's being tested)

- Both MAX17048s have the fixed address **0x36**, so they can't share a bus.
  U5 routes the MCU's single SCL/SDA pair to one of them, chosen by
  **SELECT**:
  - SELECT **low** → NC side → SCL1/SDA1 → **U9 → BAT2**
  - SELECT **high** → NO side → SCL2/SDA2 → **U4 → BAT1**
- **STATUS** is the LTC4412 STAT pin, pulled up to VOUT by R7 (470k). STAT
  sinks 10 µA once BAT2 (through CR2) is feeding the load, which pulls STATUS
  low. So STATUS **high = load on BAT1**, **low = load on BAT2**.
- U5, both gauges, and the STATUS pull-up all run off the board's own rails
  (VOUT from the MCP1700, gauges from their cells). **At least one battery
  must be connected** or nothing on the I2C side will respond.
- The board has **no I2C pull-ups** and leaves the gauges' ALRT pins
  unconnected.

## Wiring

Connect the MCU to header **JP1** on the charger board:

| JP1 pin | Net | nRF52820 (DK) | XIAO ESP32C6 | Notes |
|---|---|---|---|---|
| 1 | STATUS | P0.20 | D7 (GPIO17) | Input, **no internal pull** |
| 2 | SELECT | P0.17 | D6 (GPIO16) | Output |
| 3 | VOUT | — | — | Pull-up rail (see below), otherwise leave open |
| 4 | GND | GND | GND | **Required**: common ground |
| 5 | SCL | P0.29 | D5 (GPIO23) | 4.7k pull-up to 3V3 |
| 6 | SDA | P0.28 | D4 (GPIO22) | 4.7k pull-up to 3V3 |
| 7 | DP+ | — | — | USB data, not used by this test |
| 8 | DP- | — | — | USB data, not used by this test |
| 9 | BAT2 | — | — | Optional: probe with a meter to check VCELL |
| 10 | BAT1 | — | — | Optional: probe with a meter to check VCELL |

**Pull-ups:** the board has none, so both builds enable the MCU's internal
pull-ups (~45k on the ESP32-C6, ~13k on the nRF). That's enough at 100 kHz
with short wires. If the bus is flaky, add 4.7k from SCL and from SDA to the
MCU's own supply (XIAO 3V3 or DK VDD), **not** to GND: a resistor to GND
holds the bus low and nothing will answer.

**Supply voltages:** keep the MCU on its own USB power for this test. VOUT
is 3.3 V and the nRF52833 DK runs at 3.0 V by default, so don't tie the
DK's VDD to VOUT. The only VOUT-referenced signal the MCU sees is STATUS,
through 470k, which is harmless.

**nRF52820 pin choice:** P0.28/P0.29 are Zephyr's default i2c0 pins for this
board. P0.17/P0.20 are free once spi1 is disabled (the overlay does that).
All four exist on the real nRF52820 QFN40 package, so the same pins carry
over to a custom nRF52820 board.

**ESP32-C6 pin choice:** D4/D5 are the same I2C pins temp-max30205 and
hr-max30102-ble use, so the gauges can share that bus with the other sensors
(0x36 doesn't collide with them). D6/D7 avoid the strapping pins and leave
D0–D2 (ADC-capable) free. Note that test-base-station uses D1–D5 for the
radio, so merging with LoRa will need a pin re-plan.

## Build/upload/monitor

Setup (VS Code + pioarduino, drivers, ports) is the same as
[tools/bringup/hr-max30102-ble](../hr-max30102-ble/CLAUDE.md).

**ESP32-C6:**
```
pio run -e xiao_c6 -t upload
pio device monitor -e xiao_c6
```

**nRF52820:** set SW6 on the nRF52833 DK to `DEFAULT`. Upload goes over the
DK's onboard J-Link (install
[SEGGER J-Link](https://www.segger.com/downloads/jlink/)), and the console
comes out on the DK's VCOM USB serial port at 115200.
```
pio run -e nrf52820 -t upload
pio device monitor -e nrf52820
```
The first `nrf52820` build downloads Zephyr and all its modules, which takes
**several GB of disk** and a few minutes.

Using a custom nRF52820 board instead of the DK: it needs a Zephyr board
definition, and the overlay gets renamed to match. With no VCOM, use the
J-Link RTT console: add `CONFIG_USE_SEGGER_RTT=y`, `CONFIG_RTT_CONSOLE=y`,
and `CONFIG_UART_CONSOLE=n` to [zephyr/prj.conf](zephyr/prj.conf).

## Output

Lines starting with `#` are diagnostics. Everything else is CSV.

On boot it runs a self-test:

1. **Power path**: reads STATUS and reports which battery is feeding the load.
2. **Bus scan on each switch position**: expect only `0x36` on each side.
3. **Gauge identity**: reads VERSION (expects `0x001X`), CONFIG, ID, and
   STATUS for each gauge. If the RI (reset) flag is set, it reports and
   clears it.
4. **Switch isolation**: because both gauges are at 0x36, a stuck SELECT line
   would let one gauge answer for both. The test writes a different tag into
   each gauge's VALRT register, reads both back, and then restores the
   defaults. If the tags don't match, it isn't reaching two separate chips.
   VALRT only drives the unconnected ALRT pin, so this has no side effects.

Then once a second:

```
t_s,bat1_v,bat1_soc,bat1_crate,bat1_flags,bat2_v,bat2_soc,bat2_crate,bat2_flags,supply,path_check
12,3.912,78.41,-1.04,-,3.874,74.02,0.00,-,BAT1,ok
```

- `*_v`: cell voltage (VCELL, 1 mV shown, 78 µV raw resolution)
- `*_soc`: state of charge, %
- `*_crate`: charge rate, %/hr (positive = charging, negative = discharging)
- `*_flags`: MAX17048 STATUS bits that are set: `RI` reset, `VH`/`VL`
  voltage alert high/low, `VR` voltage reset, `HD` SOC < 1%, `SC` SOC
  changed 1%; `-` if none
- `supply`: which battery STATUS says is feeding the load
- `path_check`: `ok`, or a flag if the measured voltages contradict STATUS.
  BAT2 should only take over once it's roughly a Schottky drop above BAT1.
  `BAT2_BUT_BAT1_HIGHER` means STATUS is low while BAT1 is ≥50 mV higher.
  `BAT1_BUT_BAT2_HIGHER` means STATUS is high while BAT2 is ≥400 mV higher.

A gauge that doesn't respond prints `ERR` in its four columns, with a `#`
note when it drops out or comes back.

## Suggested test procedure

1. Connect one battery to each side, leave USB unplugged, and confirm the
   self-test passes. With a meter on JP1 pins 9/10, check that `bat1_v` and
   `bat2_v` match within about 20 mV.
2. Swap which battery is more charged (or remove BAT1) and watch `supply`
   change to BAT2.
3. Plug in USB and confirm `*_crate` goes positive on both cells as the two
   MCP73831s charge.
4. Disconnect one battery mid-run and confirm that side goes to `ERR` and
   comes back with an `RI` note when it's reconnected.

SOC from a freshly reset MAX17048 is an estimate from the first voltage
reading. It settles over the first few minutes, especially while charging.
