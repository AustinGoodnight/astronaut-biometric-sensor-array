# LoRa Config UI

A local web UI + Node server that lets you change spreading factor,
bandwidth, and TX power on the tx/rx boards **while they keep running** —
no reflash needed. It talks to each board over the same USB serial
connection you'd otherwise use for `pio device monitor`.

## How it works

The firmware (`../src/main_tx.cpp`, `../src/main_rx.cpp`) listens for
plain-text commands on Serial alongside its normal debug output:

- `SET sf=9,bw=125000,pwr=17` — applies any subset of these live via
  `LoRa.setSpreadingFactor()` / `setSignalBandwidth()` / `setTxPower()`
  (the rx board ignores `pwr`, since it doesn't transmit)
- `GET` — makes the board print its current settings

Both commands are acknowledged with a line like:
```
CFG sf=9 bw=125000 pwr=17
```

This server opens a serial connection per board (baud 115200, matching
`monitor_speed` in `platformio.ini`), forwards commands from the browser,
and streams the board's serial output back over a WebSocket so you can
watch packets/RSSI/config changes live.

## Prerequisites

- Both boards flashed with the current firmware (`pio run -e tx -t upload`
  / `pio run -e rx -t upload` from `firmware/lora-testing/`) — older
  firmware won't understand these commands.
- Node.js 18+.

## Running

```
cd firmware/lora-testing/config-ui
npm install
npm start
```

Then open http://localhost:4173. Each board gets its own panel: pick its
serial port from the dropdown, click Connect, then adjust SF/bandwidth
(and power, for tx) and click Apply.

If you only have one board plugged in at a time, connect/adjust it, then
unplug and repeat for the other — you don't need both connected
simultaneously, though the UI supports it if you have two USB ports free.

## Notes

- SF6 isn't offered — the `LoRa` library requires implicit-header mode
  for it, which this firmware doesn't set up.
- Bandwidth options are the fixed set the SX127x radio supports; anything
  else is rejected by the firmware.
- Both ends of the link must use the same SF and bandwidth to talk to
  each other — the UI doesn't enforce that for you.
