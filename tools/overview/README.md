# overview

One page that shows heart rate, respiration, skin temperature and GSR side
by side. LoRa/RF stays on its own page. It uses the same design and theme as the
RF ([range-app](../test-base-station/range-app)) and HR
([ppg_dashboard.html](../bringup/hr-max30102-ble/ppg_dashboard.html)) pages
and shares their side menu. It's a single HTML file with no build step,
published to GitHub Pages at `/overview/`.

For detail on one sensor, use that sensor's own page. This page is the
summary.

## Status

One nRF board sends all four sensors over one Bluetooth link: the Zephyr
wrist prototype in [zephyr-poc-app](../bringup/zephyr-poc-app), advertising
as "Wrist-POC". The **Demo** button in the bottom bar builds the same binary
payload, so it runs through the same decoder as the board.

## Binary payload (what the board sends)

The page connects to any board with the Nordic UART Service (same UUIDs as
the RF and HR firmware) and subscribes to its TX characteristic. A
notification of exactly **15 bytes** is decoded as the wrist board's payload,
sent every 2 s. Fields are bit-packed MSB-first (bit 0 is the top bit of
byte 0, and is reserved). The layout is defined by `PAYLOAD_LEN` and the
`F_*` constants in the firmware's `src/main.c` and mirrored by `F` in
`index.html`; change both together.

| Bits | Field | Width | Unit | Dashboard shows |
|---|---|---|---|---|
| 0 | reserved | 1 | | |
| 1–9 | Heart rate | 9 | bpm, 0 = no pulse | as is |
| 10–17 | Respiration | 8 | breaths/min | as is |
| 18–28 | Skin resistance | 11 | kΩ, 0 = no reading, 2047 = off scale | GSR in µS (1000 / kΩ) |
| 29–39 | Skin temp | 11 | °F × 10 | °C (and °F) |
| 40–71 | Latitude | 32 | degrees × 10⁷, signed | log only |
| 72–103 | Longitude | 32 | degrees × 10⁷, signed | log only |
| 104–112 | Steps | 9 | count | log only |

Example: HR 72, resp 20, 150 kΩ, 95.4 °F, lat/lon 0, 145 steps is
`12 05 04 B3 BA 00 00 00 00 00 00 00 00 48 80`.

## Text lines (older bring-up boards)

Any notification that isn't 15 bytes is read as newline-terminated text
lines, which can be split across notifications. Any mix of these lines
works; others are ignored:

| Sensor | Line | Based on |
|---|---|---|
| Heart rate | `status: ir=120000 avgBpm=72 led=47 acdc=0.80% r=0.85 fs=100.0` (`avgBpm=--` = no pulse) | [hr-max30102-ble](../bringup/hr-max30102-ble) status line |
| Respiration | `resp,15.5` (breaths/min) | new; the [Zephyr app](../bringup/zephyr-main-app) already computes it (`chan_resp`) |
| Skin temp | `33.625,92.525` (`temp_c,temp_f`) | [temp-max30205](../bringup/temp-max30205) output |
| GSR | `gsr,4.82` (microsiemens) | new |

These reuse the bring-up boards' formats so the parsing is already written.
The HR bring-up board sends its status lines
without a newline, so it won't show here until a newline is added.

## Status colours

- Heart rate: amber below 50 or above 120 bpm, red below 40 or above 160.
- Respiration: amber below 10 or above 24 br/min, red below 8 or above 30.
- Skin temp: amber below 30 or above 37 °C, red below 28 or above 38.5.
- GSR: no thresholds yet.

A reading older than 10 s is shown as stale and drops out of recordings.

## Recording

The record button saves one row per second with the latest fresh value of
each sensor: `time_iso, epoch_ms, hr_bpm, hr_r, resp_brpm,
skin_temp_c, gsr_us`. The download button saves it as CSV. Recordings are
kept in memory only, so save before closing the tab.
