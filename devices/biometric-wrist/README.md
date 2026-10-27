# biometric-wrist

**Purpose:** Wrist-worn biometric node. Measures heart rate, perspiration
(GSR), respiration, skin temperature and estimated core temperature, and
sends periodic summary records over BLE to the
[head-module](../head-module).

**MCU:** nRF52840, Zephyr / nRF Connect SDK v3.4.0. Development is on a
Seeed XIAO nRF52840; the custom board uses a Raytac nRF52840 module.

## Layout

- `firmware/`: the shipping firmware, with its own build setup. If this
  device ever has more than one MCU target, split it into `firmware/<mcu>/`.
- `tests/`: unit tests for this device only. Tests that need more than one
  device go in the top-level [tests/](../../tests).
- Schematics, PCB and enclosure: [hardware/biometric-wrist](../../hardware/biometric-wrist).

## Build and flash

The final firmware will be Zephyr / nRF Connect SDK. Until it lands in
`firmware/`, build the prototype as described in
[tools/bringup/zephyr-main-app/README.md](../../tools/bringup/zephyr-main-app/README.md).

## Status

No shipping firmware yet. Prototypes are in [tools/bringup](../../tools/bringup):

- [zephyr-main-app](../../tools/bringup/zephyr-main-app): Zephyr skeleton of
  this firmware (threads, 5 s records, flash log, BLE). It builds; drivers and
  algorithms are still stubs. It moves to `firmware/` once it runs on real
  hardware.
- Sensor bring-ups on the ESP32-C6:
  [hr-max30102-ble](../../tools/bringup/hr-max30102-ble),
  [hr-max30102-wifi](../../tools/bringup/hr-max30102-wifi),
  [temp-max30205](../../tools/bringup/temp-max30205),
  [battery-sense](../../tools/bringup/battery-sense).
