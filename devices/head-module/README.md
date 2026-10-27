# head-module

**Purpose:** The head transmitter. Collects data from the
[biometric-wrist](../biometric-wrist) node and the
[environmental-body](../environmental-body) nodes, packages it with GPS,
and sends it long distance over LoRa to the [base-station](../base-station),
using the byte format in [docs/protocol.md](../../docs/protocol.md).

**MCU:** TBD.

## Layout

- `firmware/`: the shipping firmware, with its own build setup. If this
  device ever has more than one MCU target, split it into `firmware/<mcu>/`.
- `tests/`: unit tests for this device only. Tests that need more than one
  device go in the top-level [tests/](../../tests).
- Schematics, PCB and enclosure: [hardware/head-module](../../hardware/head-module).

## Build and flash

TBD.

## Status

No firmware yet. The LoRa link is being tested with the TX board in
[tools/test-base-station](../../tools/test-base-station).
