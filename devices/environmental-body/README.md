# environmental-body

**Purpose:** Body-worn environmental nodes. Measure environmental factors
such as temperature and body movement, and report to the
[head-module](../head-module).

**MCU:** TBD.

## Layout

- `firmware/`: the shipping firmware, with its own build setup. If this
  device ever has more than one MCU target, split it into `firmware/<mcu>/`.
- `tests/`: unit tests for this device only. Tests that need more than one
  device go in the top-level [tests/](../../tests).
- Schematics, PCB and enclosure: [hardware/environmental-body](../../hardware/environmental-body).

## Build and flash

TBD.

## Status

No shipping firmware yet. The closest prototype is
[tools/bringup/freertos-env-sensors](../../tools/bringup/freertos-env-sensors):
FreeRTOS on a XIAO ESP32-C6, reading an SCD30 CO₂ sensor and a fast analog
channel into a LoRa payload.
