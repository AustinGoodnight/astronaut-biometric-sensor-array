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

This is the visual design. The plan is one nRF board that sends all four
sensors over one Bluetooth link; that firmware doesn't exist yet, so for now
use the **Demo** button in the bottom bar.

## Line format (what the board should send)

The page connects to any board with the Nordic UART Service (same UUIDs as
the RF and HR firmware) and reads newline-terminated text lines from its TX
characteristic. Lines can be split across notifications. Any mix of these
lines works; others are ignored:

| Sensor | Line | Based on |
|---|---|---|
| Heart rate | `status: ir=120000 avgBpm=72 led=47 acdc=0.80% r=0.85 fs=100.0` (`avgBpm=--` = no pulse) | [hr-max30102-ble](../bringup/hr-max30102-ble) status line |
| Respiration | `resp,15.5` (breaths/min) | new; the [Zephyr app](../bringup/zephyr-main-app) already computes it (`chan_resp`) |
| Skin temp | `33.625,92.525` (`temp_c,temp_f`) | [temp-max30205](../bringup/temp-max30205) output |
| GSR | `gsr,4.82` (microsiemens) | new |

These reuse the bring-up boards' formats so the parsing is already written.
The HR bring-up board sends its status lines
without a newline, so it won't show here until a newline is added. When the real packet format is settled in
[docs/protocol.md](../../docs/protocol.md), change the parser
(`handleLine()`) to match.

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
