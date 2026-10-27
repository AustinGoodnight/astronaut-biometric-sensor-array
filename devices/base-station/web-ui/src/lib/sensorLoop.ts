// Server-side "sensor" — ticks once a second for as long as the Node
// process is alive, independent of which page (if any) is open in the
// browser. The device is always live (matching the "Device Telemetry ·
// ACTIVE" badge in the UI) — it keeps walking every second whether or not
// a session is being recorded. A session just taps into that one live
// feed and persists each tick to the DB for as long as it's active.
import { addReading, getActiveSessions, getUserThresholds } from "./db";
import { METRICS } from "./metrics";

interface WalkConfig {
  base: number;
  jitter: number;
  min: number;
  max: number;
}

const WALK: Record<string, WalkConfig> = {
  hr: { base: 72, jitter: 1.4, min: 58, max: 104 },
  perspiration: { base: 0.4, jitter: 0.025, min: 0.12, max: 0.95 },
  skinTemp: { base: 33.2, jitter: 0.04, min: 31.5, max: 34.6 },
  coreTemp: { base: 38.3, jitter: 0.035, min: 37.4, max: 39.2 },
  respiration: { base: 16, jitter: 0.6, min: 10, max: 24 },
};

const DECIMALS: Record<string, number> = Object.fromEntries(METRICS.map((m) => [m.key, m.decimals]));
const HISTORY_LEN = 30;

const deviceCurrent: Record<string, number> = {};
const deviceHistory: Record<string, number[]> = {};
for (const key of Object.keys(WALK)) {
  deviceCurrent[key] = WALK[key].base;
  deviceHistory[key] = new Array(HISTORY_LEN).fill(WALK[key].base);
}

function tick() {
  for (const key of Object.keys(WALK)) {
    const w = WALK[key];
    const v = deviceCurrent[key];
    const drift = (w.base - v) * 0.04;
    const noise = (Math.random() - 0.5) * 2 * w.jitter;
    const next = Math.min(w.max, Math.max(w.min, v + drift + noise));
    const rounded = Number(next.toFixed(DECIMALS[key] ?? 2));
    deviceCurrent[key] = rounded;
    const h = deviceHistory[key];
    h.push(rounded);
    if (h.length > HISTORY_LEN) h.shift();
  }

  const sessions = getActiveSessions();
  if (sessions.length === 0) return;

  const ts = Date.now();
  for (const session of sessions) {
    const thresholds = getUserThresholds(session.user);
    const elevated = Object.keys(WALK).some((key) => {
      const [low, high] = thresholds[key];
      return deviceCurrent[key] < low || deviceCurrent[key] > high;
    });
    addReading(session.id, ts, deviceCurrent, elevated);
  }
}

/** The device's current live values + rolling history, for /api/live. */
export function getLiveSnapshot(): { current: Record<string, number>; history: Record<string, number[]> } {
  return {
    current: { ...deviceCurrent },
    history: Object.fromEntries(Object.entries(deviceHistory).map(([k, v]) => [k, v.slice()])),
  };
}

declare global {
  // eslint-disable-next-line no-var
  var __sensorLoopInterval: ReturnType<typeof setInterval> | undefined;
}

// Replace (rather than skip) any previous interval so this stays correct
// across Vite's dev-mode HMR reloads of this module — a boolean "already
// started" guard would survive the reload via globalThis and permanently
// block the new module instance's loop from ever starting, silently
// freezing the live feed after any edit to this file.
if (globalThis.__sensorLoopInterval) clearInterval(globalThis.__sensorLoopInterval);
globalThis.__sensorLoopInterval = setInterval(tick, 1000);
