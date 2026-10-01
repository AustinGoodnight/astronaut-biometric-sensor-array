# LoRa TX/RX Link — XIAO ESP32C6 + Wio-SX1262

A port of [../lora-testing](../lora-testing) to [Seeed XIAO ESP32C6](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/)
boards with the **Wio-SX1262 for XIAO** LoRa module. Same behavior: the
transmitter sends a counter/dummy sensor packet every 2 seconds at 915 MHz,
and the receiver prints each packet plus its RSSI/SNR to serial.

| | |
|---|---|
| **Board**    | Seeed XIAO ESP32C6 |
| **Radio**    | Wio-SX1262 (SX1262, via [`jgromes/RadioLib`](https://github.com/jgromes/RadioLib)) |
| **Frequency**| 915 MHz |
| **TX code**  | [src/main_tx.cpp](src/main_tx.cpp) |
| **RX code**  | [src/main_rx.cpp](src/main_rx.cpp) |

## Wiring

The Wio-SX1262 for XIAO plugs straight onto the XIAO headers. Pins are
defined at the top of each `main_*.cpp`:

| Signal | XIAO pin |
|--------|----------|
| NSS (CS) | D4 |
| DIO1 (IRQ) | D1 |
| NRST     | D2 |
| BUSY     | D3 |
| RF_SW    | D5 |
| SPI (SCK/MISO/MOSI) | D8 / D9 / D10 (default hardware SPI) |

The module's TCXO is powered from DIO3 at 1.8 V and DIO2 drives the RF
switch — both configured in `setup()`.

Note: D4/D5 are also the XIAO's default I2C pins, so an I2C sensor can't
share those pins with this module.

## Quick start

Identical to [../lora-testing](../lora-testing/README.md): open this folder
in VS Code with the pioarduino extension, pick the `tx` or `rx`
environment, and build/upload/monitor — or from the CLI:
```
pio run -e tx -t upload
pio device monitor -e tx
```
Repeat for the second board with `rx`. With both boards plugged in at once,
copy `platformio.local.ini.example` to `platformio.local.ini` and set
the ports (see [../lora-testing/CLAUDE.md](../lora-testing/CLAUDE.md#local-port-overrides)).

## Live config

The firmware speaks the same `SET sf=..,bw=..,pwr=..` / `GET` serial
protocol (also over BLE, see below) as the C3 boards, so [../lora-testing/config-ui](../lora-testing/config-ui)
works with these boards unchanged. The SX1262 accepts TX power from -9 to
22 dBm (the UI slider covers 2–20).

## Bluetooth + range-test app

Both boards also advertise over BLE as **`LoRa-TX`** / **`LoRa-RX`**
([lib/BleLink](lib/BleLink)). Every serial line is mirrored to the phone,
and the phone can send the same `SET`/`GET` commands.

[range-app/index.html](range-app/index.html) is a single-page web app for
field range tests, made for **Bluefy** on iOS (Safari has no Web
Bluetooth). It connects to a board over BLE, tags every packet with a
high-accuracy GPS fix, and computes distance from a fixed anchor.

- **Logging bar** — floats above the tab bar on every tab: current
  session, row count, **New** session and **Start/Stop** logging.
- **Home** — dashboard of everything at once: live RSSI/SNR, distance,
  time since last packet, delivery %, a mini map, RSSI
  and delivery vs distance, anchor + your position, export, and recent
  board output. Each card links to its full tab.
- **Board** — radio settings (SF / bandwidth / TX power) and the full
  board output.
- **Map** — every packet as a dot colored by RSSI, missed packets as
  hollow dots, the anchor as a star with distance rings.
- **Charts** — RSSI and SNR vs distance, delivery % per distance band,
  RSSI over time.
- **Location** — set the anchor by pasting coordinates (or a Google/Apple
  Maps link), or measure your position as a 30 s weighted average and
  share it with the other person.
- **Data** — export everything as CSV via the iOS share sheet, or copy it.

Missed packets are detected from gaps in the TX's `count:` field, plus a
timeout while nothing arrives (so the map shows where reception stopped).
Gaps caused by the phone's BLE link dropping or the app being backgrounded
are not counted as radio misses.

### Running a range test

1. Flash both boards. The person at the fixed spot keeps the **TX** board
   there (the default "TX fixed, RX moves" mode).
2. Both people open the app in Bluefy and allow precise location
   (iOS Settings → Bluefy → Location → *While Using* + *Precise Location*).
3. At the TX: **Location → Measure (30 s average)**, standing still at the
   board, then **Share** the coordinates to the other person (or tap
   **Set as anchor** if the same phone will walk).
4. With the RX: paste the shared text into **Location → Use pasted
   coordinates**, **Connect board** → `LoRa-RX`, then **Start** in the
   logging bar at the bottom.
   Keep the screen on while walking — iOS pauses GPS and Bluetooth when
   the phone locks.
5. **Data → Export CSV** afterwards. Tap **New** (new session) between
   configurations (different SF/power) and put the settings in the session
   note on the Data tab so they end up in every CSV row.

Phone GPS is good to about ±3–5 m in the open. Every row records the fix
accuracy and the anchor accuracy, so you can filter out bad fixes.

CSV columns: `session, time_iso, epoch_ms, board, event (received / missed /
crc_fail / sent), count, rssi_dbm, snr_db, sf, bw_hz, pwr_dbm, lat, lon,
gps_accuracy_m, altitude_m, altitude_accuracy_m, speed_mps, gps_fix_age_ms,
anchor_board, anchor_lat, anchor_lon, anchor_accuracy_m, distance_m, note,
payload`.

Web Bluetooth and precise GPS both need HTTPS, so the app is served via
GitHub Pages at
`https://austingoodnight.github.io/astronaut-biometric-sensor-array/lora-range/`
(the HR dashboard is at `/hr-testing/`; see
[../../.github/workflows/pages.yml](../../.github/workflows/pages.yml)).

## Differences from lora-testing

- RadioLib instead of `sandeepmistry/LoRa` (that library only supports SX127x).
- Packets have CRC enabled (RadioLib default), and the RX side reports SNR too.
- Changing settings over serial puts the radio in standby first (the SX1262
  requires it), so a packet on air when you hit Apply gets dropped.
- These boards are not set up to talk to the SX127x boards in lora-testing.
