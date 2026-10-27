# base-station

**Purpose:** Receives the LoRa packets from the [head-module](../head-module)
and passes them over USB serial to the base-station web UI.

**MCU:** TBD.

## Layout

- `firmware/`: the LoRa receiver firmware, with its own build setup.
- `web-ui/`: the base-station web UI (Astro + Node). It reads the
  base station over USB serial and shows live data, sessions and history.
  This is separate from the testing web UIs in [tools/](../../tools), which
  talk to individual nodes over BLE or serial without the LoRa link.
- `tests/`: unit tests for this device only. Tests that need more than one
  device go in the top-level [tests/](../../tests).
- Schematics, PCB and enclosure: [hardware/base-station](../../hardware/base-station).

## Build and flash

- Firmware: TBD.
- Web UI: from `web-ui/`, run `npm install`, then `npm run dev`.

## Status

- Firmware: none yet. The bench stand-in is the RX board in
  [tools/test-base-station](../../tools/test-base-station).
- Web UI: runs on simulated vitals; not yet fed by the base station.
