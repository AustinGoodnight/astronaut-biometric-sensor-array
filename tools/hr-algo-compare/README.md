# hr-algo-compare

One web page ([index.html](index.html)) that runs both heart-rate algorithms
from [zephyr-poc-app](../bringup/zephyr-poc-app) side by side and shows what
each one does every second:

- **Spectral** ([spectral_hr.c](../bringup/zephyr-poc-app/src/max/spectral_hr.c)):
  the 8 s spectrum, the accelerometer peaks that get notched out, and the
  tracker's choice compared with the biggest bin.
- **Classic** ([classic_hr.c](../bringup/zephyr-poc-app/src/max/classic_hr.c)):
  the bandpassed signal, the autocorrelation with its 0.5 and 85% thresholds,
  and what got published (median of the last 5, "--" after 5 s without a good
  window). Classic doesn't remove motion; it's the still-wrist algorithm.

It's published on GitHub Pages at `/hr-compare/` (see
[.github/workflows/pages.yml](../../.github/workflows/pages.yml)). Locally,
serve this folder over http (`python3 -m http.server`) and open
`http://localhost:8000`: Web Serial needs https or localhost.

## Inputs

- **Connect USB** (Chrome or Edge): the zephyr-poc-app board in bench mode, the
  same serial protocol as the HR testing page (`R` to start the stream, then
  `raw,` / `mc,` / `status:` lines). The `mc,` lines carry the accelerometer, so
  motion removal only works with the Sense build. The board's own numbers from
  its status line are drawn dashed next to the page's.
- **Load CSV**: a recording with an `ir` column at 100 Hz, plus optional
  `ax,ay,az` in m/s². The HR testing page's recordings work (no accelerometer).
  `?csv=<url>` loads one when the page opens.
- **Save CSV** writes everything received as `ir,red,ax,ay,az`, which the
  plotting scripts in [zephyr-poc-app/scripts](../bringup/zephyr-poc-app/scripts) also read.

The settings (spectral notch width, spectral max change)
only change the page's copy, and re-run everything received so far. A
"tuned" badge shows when they differ from the firmware.

## Keeping it in sync with the firmware

The algorithms are ported by hand. The block between `BEGIN-ALGO` and
`END-ALGO` in index.html must match the C. It was checked
second-by-second against the Python ports in `zephyr-poc-app/scripts/`, which were
checked against the C compiled on a PC. If you change either algorithm in the
firmware, change it here and in the Python scripts too. With a live board,
the "page − board" note under **Board reports** shows any drift. It should
read 0 once both have been running for about 15 s.
