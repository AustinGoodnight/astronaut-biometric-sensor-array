# base-station web UI

Astro + Node dashboard for the operator at the base station: live vitals,
recording sessions, per-user thresholds and session history. Readings are
stored in a local SQLite database (`data/vitals.db`, created on first run,
not committed).

Status: runs on simulated vitals (`src/scripts/vitalsSim.ts`). It is not yet
fed by the base-station receiver over USB serial.

## Run

```sh
npm install
npm run dev      # dev server
npm run build    # production build (Node adapter)
```
