// Client-side simulation standing in for a real sensor feed.
// Ticks once a second: random-walks each vital toward its baseline,
// updates any [data-live="<key>"] element in the DOM, and broadcasts
// a `vitals:tick` CustomEvent so charts can redraw themselves.

export interface MetricConfig {
  key: string;
  base: number;
  jitter: number;
  min: number;
  max: number;
  decimals: number;
  /** value is "elevated" outside this range */
  normal: [number, number];
}

export const METRICS: MetricConfig[] = [
  { key: "hr", base: 72, jitter: 1.4, min: 58, max: 104, decimals: 0, normal: [60, 92] },
  { key: "perspiration", base: 0.4, jitter: 0.025, min: 0.12, max: 0.95, decimals: 2, normal: [0.2, 0.7] },
  { key: "skinTemp", base: 33.2, jitter: 0.04, min: 31.5, max: 34.6, decimals: 1, normal: [32, 34.2] },
  { key: "coreTemp", base: 38.3, jitter: 0.035, min: 37.4, max: 39.2, decimals: 1, normal: [36.8, 38.2] },
  { key: "respiration", base: 16, jitter: 0.6, min: 10, max: 24, decimals: 0, normal: [12, 20] },
  { key: "latency", base: 12, jitter: 4, min: 6, max: 26, decimals: 0, normal: [0, 100] },
];

const HISTORY_LEN = 30;

type State = Record<string, number>;
type History = Record<string, number[]>;

const current: State = {};
const history: History = {};

for (const m of METRICS) {
  current[m.key] = m.base;
  history[m.key] = new Array(HISTORY_LEN).fill(m.base);
}

function step(m: MetricConfig): number {
  const v = current[m.key];
  const drift = (m.base - v) * 0.04;
  const noise = (Math.random() - 0.5) * 2 * m.jitter;
  const next = Math.min(m.max, Math.max(m.min, v + drift + noise));
  current[m.key] = next;
  const h = history[m.key];
  h.push(next);
  if (h.length > HISTORY_LEN) h.shift();
  return next;
}

function paint() {
  for (const m of METRICS) {
    const v = current[m.key];
    const text = v.toFixed(m.decimals);
    document.querySelectorAll<HTMLElement>(`[data-live="${m.key}"]`).forEach((el) => {
      el.textContent = text;
    });
  }

  const elevated = METRICS.filter(
    (m) => m.key !== "latency" && (current[m.key] < m.normal[0] || current[m.key] > m.normal[1])
  ).length;
  document.querySelectorAll<HTMLElement>('[data-live="elevatedCount"]').forEach((el) => {
    el.textContent = String(elevated);
  });

  document.querySelectorAll<HTMLElement>("[data-live-state]").forEach((el) => {
    const state = elevated === 0 ? "good" : elevated === 1 ? "okay" : "bad";
    el.querySelectorAll<HTMLElement>("[data-state]").forEach((pill) => {
      const isActive = pill.dataset.state === state;
      pill.classList.toggle("bg-ink-black", isActive);
      pill.classList.toggle("text-white", isActive);
      pill.classList.toggle("text-muted", !isActive);
    });
  });

  window.dispatchEvent(
    new CustomEvent("vitals:tick", { detail: { current: { ...current }, history: cloneHistory() } })
  );
}

function cloneHistory(): History {
  const out: History = {};
  for (const k in history) out[k] = history[k].slice();
  return out;
}

let started = false;

/**
 * Seeds the sim's current values + rolling history from real recent readings
 * (e.g. the active session's last readings from the DB) instead of the flat
 * baseline, so the live chart continues where it left off across page
 * navigations instead of visually resetting.
 */
export interface VitalsSeed {
  current?: Record<string, number | null | undefined>;
  history?: Record<string, (number | null | undefined)[]>;
}

export interface VitalsSimOptions {
  /**
   * Poll the server's /api/live endpoint for each tick's values instead of
   * generating a local random walk — used on pages backed by a real
   * recording session, so what's displayed is exactly what's being
   * persisted server-side (see src/lib/sensorLoop.ts) and keeps updating
   * even though that loop runs independently of this page being open.
   * `latency` has no server-side reading and keeps ticking locally either way.
   */
  poll?: boolean;
}

export function initVitalsSim(seed?: VitalsSeed, opts?: VitalsSimOptions) {
  if (started) return;
  started = true;

  if (seed) {
    for (const m of METRICS) {
      const seedHist = seed.history?.[m.key]?.filter((v): v is number => v != null && Number.isFinite(v));
      if (seedHist && seedHist.length > 0) {
        const h = seedHist.slice(-HISTORY_LEN);
        while (h.length < HISTORY_LEN) h.unshift(h[0]);
        history[m.key] = h;
        current[m.key] = h[h.length - 1];
      }
      const seedCurrent = seed.current?.[m.key];
      if (seedCurrent != null && Number.isFinite(seedCurrent)) {
        current[m.key] = seedCurrent;
      }
    }
  }

  paint();

  if (opts?.poll) {
    const latency = METRICS.find((m) => m.key === "latency");
    setInterval(async () => {
      if (latency) step(latency);
      try {
        const res = await fetch("/api/live");
        const data = await res.json();
        if (data.current) {
          for (const m of METRICS) {
            if (m.key === "latency") continue;
            const v = data.current[m.key];
            if (v == null || !Number.isFinite(v)) continue;
            current[m.key] = v;
            const h = history[m.key];
            h.push(v);
            if (h.length > HISTORY_LEN) h.shift();
          }
        }
      } catch {}
      paint();
    }, 1000);
  } else {
    setInterval(() => {
      for (const m of METRICS) step(m);
      paint();
    }, 1000);
  }
}

export function getMetrics() {
  return METRICS;
}

/**
 * Overrides each metric's "normal" range (e.g. with a specific user's
 * personal thresholds) and repaints immediately so elevated-count,
 * status badges, etc. reflect the change without waiting for the next tick.
 */
export function setThresholds(thresholds: Record<string, [number, number]>) {
  for (const m of METRICS) {
    const t = thresholds[m.key];
    if (t) m.normal = t;
  }
  if (started) paint();
  window.dispatchEvent(new CustomEvent("thresholds:changed", { detail: { ...thresholds } }));
}
