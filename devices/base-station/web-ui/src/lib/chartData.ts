import type { ReadingRow } from "./db";

/** Buckets readings down to at most `target` points, averaging each bucket's metrics. */
export function downsample(rows: ReadingRow[], target: number): ReadingRow[] {
  if (rows.length <= target) return rows;
  const bucketSize = rows.length / target;
  const out: ReadingRow[] = [];
  for (let i = 0; i < target; i++) {
    const start = Math.floor(i * bucketSize);
    const end = Math.max(start + 1, Math.floor((i + 1) * bucketSize));
    const bucket = rows.slice(start, end);
    const avg = (key: keyof ReadingRow) => bucket.reduce((a, r) => a + (Number(r[key]) || 0), 0) / bucket.length;
    out.push({
      ...bucket[Math.floor(bucket.length / 2)],
      hr: avg("hr"),
      perspiration: avg("perspiration"),
      skin_temp: avg("skin_temp"),
      core_temp: avg("core_temp"),
      respiration: avg("respiration"),
    });
  }
  return out;
}

/**
 * Buckets readings into `target` equal-width *time* slices spanning the full
 * range (not equal-*count* slices) — important when reading density varies a
 * lot across the span, e.g. a dense recent recording vs. sparse older history.
 * Empty slices carry the previous slice's values forward so the line stays
 * continuous instead of dropping out.
 */
export function downsampleByTime(rows: ReadingRow[], target: number): ReadingRow[] {
  if (rows.length === 0) return [];
  if (rows.length === 1) return rows;

  const firstTs = rows[0].ts;
  const lastTs = rows[rows.length - 1].ts;
  const span = lastTs - firstTs || 1;
  const bucketSpan = span / target;

  const buckets: ReadingRow[][] = Array.from({ length: target }, () => []);
  for (const r of rows) {
    const idx = Math.min(target - 1, Math.floor((r.ts - firstTs) / bucketSpan));
    buckets[idx].push(r);
  }

  const avg = (bucket: ReadingRow[], key: keyof ReadingRow) =>
    bucket.reduce((a, r) => a + (Number(r[key]) || 0), 0) / bucket.length;

  const out: ReadingRow[] = [];
  let last: ReadingRow | null = null;
  for (let i = 0; i < target; i++) {
    const bucket = buckets[i];
    const ts = Math.round(firstTs + (i + 0.5) * bucketSpan);
    if (bucket.length === 0) {
      out.push(last ? { ...last, ts } : { ...rows[0], ts });
      continue;
    }
    const point: ReadingRow = {
      ...bucket[bucket.length - 1],
      ts,
      hr: avg(bucket, "hr"),
      perspiration: avg(bucket, "perspiration"),
      skin_temp: avg(bucket, "skin_temp"),
      core_temp: avg(bucket, "core_temp"),
      respiration: avg(bucket, "respiration"),
    };
    out.push(point);
    last = point;
  }
  return out;
}

/** Sparse "start / mid / end"-style labels for a fixed-length axis. */
export function sparseLabels(n: number, endLabel: string = "end"): string[] {
  if (n <= 1) return [endLabel];
  const labels = new Array(n).fill("");
  labels[0] = "start";
  labels[n - 1] = endLabel;
  if (n >= 5) labels[Math.floor((n - 1) / 2)] = "mid";
  return labels;
}

/** Sparse labels using real elapsed-time-from-now phrasing, e.g. "3d ago" / "6h ago" / "12m ago". */
export function elapsedLabels(timestamps: number[], now: number = Date.now()): string[] {
  const n = timestamps.length;
  if (n <= 1) return ["now"];

  function relative(ts: number): string {
    const ms = now - ts;
    if (ms < 60_000) return "now";
    const minutes = Math.round(ms / 60_000);
    if (minutes < 60) return `${minutes}m ago`;
    const hours = Math.round(ms / 3_600_000);
    if (hours < 24) return `${hours}h ago`;
    const days = Math.round(ms / 86_400_000);
    return `${days}d ago`;
  }

  const labels = new Array(n).fill("");
  labels[0] = relative(timestamps[0]);
  labels[n - 1] = "now";
  if (n >= 5) {
    const mid = Math.floor((n - 1) / 2);
    labels[mid] = relative(timestamps[mid]);
  }
  return labels;
}

export const DB_KEY: Record<string, keyof ReadingRow> = {
  hr: "hr",
  perspiration: "perspiration",
  skinTemp: "skin_temp",
  coreTemp: "core_temp",
  respiration: "respiration",
};
