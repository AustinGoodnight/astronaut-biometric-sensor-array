# Web UI

Web dashboard for monitoring/visualizing the LoRa link (e.g. live RSSI,
packet history) fed by [firmware/lora-testing](../firmware/lora-testing)
and/or [signal-processing](../signal-processing) output.

Nothing here yet — no framework has been picked. When code lands, this
README should cover:

- Framework/stack and Node version
- How to install deps and run the dev server (`npm install`, `npm run dev`,
  etc.)
- Where it gets its data from (live serial/API, or processed files from
  signal-processing)

Add a `.gitignore` here for the chosen stack (e.g. `node_modules/`,
`dist/`, `.env`) — the root `.gitignore` intentionally only covers
cross-cutting OS/editor files, not subsystem tooling.
