import Database from "better-sqlite3";
import path from "node:path";
import fs from "node:fs";
import { METRICS } from "./metrics";

const DATA_DIR = path.join(process.cwd(), "data");
if (!fs.existsSync(DATA_DIR)) fs.mkdirSync(DATA_DIR, { recursive: true });

const db = new Database(path.join(DATA_DIR, "vitals.db"));
db.pragma("journal_mode = WAL");

db.exec(`
  CREATE TABLE IF NOT EXISTS sessions (
    id TEXT PRIMARY KEY,
    label TEXT NOT NULL,
    user TEXT NOT NULL,
    started_at INTEGER NOT NULL,
    ended_at INTEGER,
    avg_hr REAL,
    flagged_count INTEGER NOT NULL DEFAULT 0,
    source TEXT NOT NULL DEFAULT 'recorded'
  );

  CREATE TABLE IF NOT EXISTS readings (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
    ts INTEGER NOT NULL,
    hr REAL,
    perspiration REAL,
    skin_temp REAL,
    core_temp REAL,
    respiration REAL,
    elevated INTEGER NOT NULL DEFAULT 0
  );

  CREATE INDEX IF NOT EXISTS idx_readings_session ON readings(session_id);

  CREATE TABLE IF NOT EXISTS users (
    name TEXT PRIMARY KEY,
    hr_low REAL, hr_high REAL,
    perspiration_low REAL, perspiration_high REAL,
    skin_temp_low REAL, skin_temp_high REAL,
    core_temp_low REAL, core_temp_high REAL,
    respiration_low REAL, respiration_high REAL
  );
`);

export interface SessionRow {
  id: string;
  label: string;
  user: string;
  started_at: number;
  ended_at: number | null;
  avg_hr: number | null;
  flagged_count: number;
  source: "seed" | "recorded";
}

export interface ReadingRow {
  id: number;
  session_id: string;
  ts: number;
  hr: number;
  perspiration: number;
  skin_temp: number;
  core_temp: number;
  respiration: number;
  elevated: number;
}

export function createSession(id: string, label: string, user: string, startedAt: number, source: "seed" | "recorded" = "recorded") {
  db.prepare(
    `INSERT INTO sessions (id, label, user, started_at, source) VALUES (?, ?, ?, ?, ?)`
  ).run(id, label, user, startedAt, source);
}

export function addReading(sessionId: string, ts: number, values: Record<string, number>, elevated: boolean) {
  db.prepare(
    `INSERT INTO readings (session_id, ts, hr, perspiration, skin_temp, core_temp, respiration, elevated)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?)`
  ).run(sessionId, ts, values.hr ?? null, values.perspiration ?? null, values.skinTemp ?? null, values.coreTemp ?? null, values.respiration ?? null, elevated ? 1 : 0);
}

export function stopSessionRow(id: string, endedAt: number): SessionRow | undefined {
  const session = getSession(id);
  const readings = db.prepare(`SELECT * FROM readings WHERE session_id = ? ORDER BY ts ASC`).all(id) as ReadingRow[];
  const avgHr = readings.length ? readings.reduce((a, r) => a + r.hr, 0) / readings.length : null;
  const ranges = session ? getUserThresholds(session.user) : defaultThresholds();
  const flaggedCount = countDistinctFlaggedMetrics(readings, ranges);
  db.prepare(`UPDATE sessions SET ended_at = ?, avg_hr = ?, flagged_count = ? WHERE id = ?`).run(endedAt, avgHr, flaggedCount, id);
  return getSession(id);
}

function countDistinctFlaggedMetrics(readings: ReadingRow[], ranges: Record<string, [number, number]>): number {
  if (readings.length === 0) return 0;
  let count = 0;
  for (const m of METRICS) {
    const col = METRIC_DB_COLUMN[m.key];
    const avg = readings.reduce((a, r) => a + (Number(r[col]) || 0), 0) / readings.length;
    if (avg < ranges[m.key][0] || avg > ranges[m.key][1]) count++;
  }
  return count;
}

function defaultThresholds(): Record<string, [number, number]> {
  return Object.fromEntries(METRICS.map((m) => [m.key, m.normal]));
}

export function getUserThresholds(name: string): Record<string, [number, number]> {
  ensureUser(name);
  const u = db.prepare(`SELECT * FROM users WHERE name = ?`).get(name) as UserRow | undefined;
  if (!u) return defaultThresholds();
  const out: Record<string, [number, number]> = {};
  for (const m of METRICS) {
    const prefix = METRIC_COLUMN_PREFIX[m.key];
    const low = (u as any)[`${prefix}_low`];
    const high = (u as any)[`${prefix}_high`];
    out[m.key] = Number.isFinite(low) && Number.isFinite(high) ? [low, high] : m.normal;
  }
  return out;
}

const METRIC_COLUMN_PREFIX: Record<string, string> = {
  hr: "hr",
  perspiration: "perspiration",
  skinTemp: "skin_temp",
  coreTemp: "core_temp",
  respiration: "respiration",
};

export function getSession(id: string): SessionRow | undefined {
  return db.prepare(`SELECT * FROM sessions WHERE id = ?`).get(id) as SessionRow | undefined;
}

export function listSessions(): SessionRow[] {
  return db.prepare(`SELECT * FROM sessions ORDER BY started_at DESC`).all() as SessionRow[];
}

/** The session currently being recorded, or — if none is active — the most recently started one. */
export function getCurrentOrLatestSession(): SessionRow | undefined {
  const active = db.prepare(`SELECT * FROM sessions WHERE ended_at IS NULL ORDER BY started_at DESC LIMIT 1`).get() as SessionRow | undefined;
  if (active) return active;
  return db.prepare(`SELECT * FROM sessions ORDER BY started_at DESC LIMIT 1`).get() as SessionRow | undefined;
}

/** Every session currently being recorded (there's normally at most one). */
export function getActiveSessions(): SessionRow[] {
  return db.prepare(`SELECT * FROM sessions WHERE ended_at IS NULL ORDER BY started_at ASC`).all() as SessionRow[];
}

export function getReadings(sessionId: string): ReadingRow[] {
  return db.prepare(`SELECT * FROM readings WHERE session_id = ? ORDER BY ts ASC`).all(sessionId) as ReadingRow[];
}

/** Every reading ever recorded, oldest first — the full device history. */
export function getAllReadingsChronological(): ReadingRow[] {
  return db.prepare(`SELECT * FROM readings ORDER BY ts ASC`).all() as ReadingRow[];
}

export const METRIC_DB_COLUMN: Record<string, keyof ReadingRow> = {
  hr: "hr",
  perspiration: "perspiration",
  skinTemp: "skin_temp",
  coreTemp: "core_temp",
  respiration: "respiration",
};

export interface UserRow {
  name: string;
  hr_low: number;
  hr_high: number;
  perspiration_low: number;
  perspiration_high: number;
  skin_temp_low: number;
  skin_temp_high: number;
  core_temp_low: number;
  core_temp_high: number;
  respiration_low: number;
  respiration_high: number;
}

export interface UserWithStats extends UserRow {
  sessionCount: number;
  flaggedTotal: number;
  averages: Record<string, number | null>;
}

const KNOWN_USERS = ["Alex Rivera", "Jordan Lee", "Sam Patel", "Casey Kim"];

function ensureUser(name: string) {
  const exists = db.prepare(`SELECT 1 FROM users WHERE name = ?`).get(name);
  if (exists) return;
  db.prepare(
    `INSERT INTO users (name, hr_low, hr_high, perspiration_low, perspiration_high, skin_temp_low, skin_temp_high, core_temp_low, core_temp_high, respiration_low, respiration_high)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`
  ).run(
    name,
    metric("hr").normal[0], metric("hr").normal[1],
    metric("perspiration").normal[0], metric("perspiration").normal[1],
    metric("skinTemp").normal[0], metric("skinTemp").normal[1],
    metric("coreTemp").normal[0], metric("coreTemp").normal[1],
    metric("respiration").normal[0], metric("respiration").normal[1]
  );
}

function metric(key: string) {
  const m = METRICS.find((m) => m.key === key)!;
  return m;
}

export function seedUsersIfNeeded() {
  // Only seed the starter names once, when the table is brand new — otherwise
  // this would silently resurrect a name right after it's renamed away.
  const count = (db.prepare(`SELECT COUNT(*) as n FROM users`).get() as { n: number }).n;
  if (count === 0) {
    for (const name of KNOWN_USERS) ensureUser(name);
  }
  // Still backfill a row for any session's user that somehow has none (defensive).
  const sessionUsers = db.prepare(`SELECT DISTINCT user FROM sessions`).all() as { user: string }[];
  for (const { user } of sessionUsers) ensureUser(user);
}

export function listUsersWithStats(): UserWithStats[] {
  seedUsersIfNeeded();
  const users = db.prepare(`SELECT * FROM users ORDER BY name ASC`).all() as UserRow[];

  return users.map((u) => {
    const sessions = db.prepare(`SELECT id FROM sessions WHERE user = ?`).all(u.name) as { id: string }[];
    const sessionCount = sessions.length;
    const flaggedTotal = (db.prepare(`SELECT COALESCE(SUM(flagged_count), 0) as n FROM sessions WHERE user = ?`).get(u.name) as { n: number }).n;

    const averages: Record<string, number | null> = {};
    if (sessionCount > 0) {
      const placeholders = sessions.map(() => "?").join(",");
      const row = db
        .prepare(
          `SELECT AVG(hr) as hr, AVG(perspiration) as perspiration, AVG(skin_temp) as skinTemp, AVG(core_temp) as coreTemp, AVG(respiration) as respiration
           FROM readings WHERE session_id IN (${placeholders})`
        )
        .get(...sessions.map((s) => s.id)) as Record<string, number | null>;
      for (const m of METRICS) averages[m.key] = row[m.key];
    } else {
      for (const m of METRICS) averages[m.key] = null;
    }

    return { ...u, sessionCount, flaggedTotal, averages };
  });
}

export function listUserNames(): string[] {
  seedUsersIfNeeded();
  return (db.prepare(`SELECT name FROM users ORDER BY name ASC`).all() as { name: string }[]).map((r) => r.name);
}

export function renameUser(oldName: string, newName: string): { ok: boolean; error?: string } {
  const trimmed = newName.trim();
  if (!trimmed) return { ok: false, error: "Name can't be empty" };
  if (trimmed === oldName) return { ok: true };

  const collision = db.prepare(`SELECT 1 FROM users WHERE name = ?`).get(trimmed);
  if (collision) return { ok: false, error: "That name is already in use" };

  ensureUser(oldName);
  const tx = db.transaction(() => {
    db.prepare(`UPDATE users SET name = ? WHERE name = ?`).run(trimmed, oldName);
    db.prepare(`UPDATE sessions SET user = ? WHERE user = ?`).run(trimmed, oldName);
  });
  tx();
  return { ok: true };
}

export function updateUserThresholds(name: string, thresholds: Record<string, [number, number]>) {
  ensureUser(name);
  const cols = ["hr", "perspiration", "skinTemp", "coreTemp", "respiration"] as const;
  const dbCols: Record<string, string> = { hr: "hr", perspiration: "perspiration", skinTemp: "skin_temp", coreTemp: "core_temp", respiration: "respiration" };
  const sets: string[] = [];
  const values: number[] = [];
  for (const key of cols) {
    if (!thresholds[key]) continue;
    sets.push(`${dbCols[key]}_low = ?`, `${dbCols[key]}_high = ?`);
    values.push(thresholds[key][0], thresholds[key][1]);
  }
  if (sets.length === 0) return;
  db.prepare(`UPDATE users SET ${sets.join(", ")} WHERE name = ?`).run(...values, name);
}

export function seedIfEmpty() {
  const count = (db.prepare(`SELECT COUNT(*) as n FROM sessions`).get() as { n: number }).n;
  if (count > 0) return;
  seedDemoSessions();
}

function seedDemoSessions() {
  const DEMO = [
    { id: "morning-run", label: "Morning Run", user: "Alex Rivera", startedAgoMin: 14 * 60, durationMin: 35, intensity: "elevated" as const, flaggedCount: 2 },
    { id: "work-session", label: "Work Session", user: "Alex Rivera", startedAgoMin: 12 * 60, durationMin: 210, intensity: "normal" as const, flaggedCount: 0 },
    { id: "afternoon-walk", label: "Afternoon Walk", user: "Alex Rivera", startedAgoMin: 26 * 60, durationMin: 33, intensity: "normal" as const, flaggedCount: 0 },
    { id: "sleep", label: "Sleep", user: "Alex Rivera", startedAgoMin: 22 * 60, durationMin: 463, intensity: "normal" as const, flaggedCount: 1 },
    { id: "evening-workout", label: "Evening Workout", user: "Jordan Lee", startedAgoMin: 47 * 60, durationMin: 45, intensity: "elevated" as const, flaggedCount: 3 },
    { id: "rest-day", label: "Rest Day", user: "Jordan Lee", startedAgoMin: 71 * 60, durationMin: 720, intensity: "normal" as const, flaggedCount: 0 },
  ];

  const now = Date.now();
  const insertSession = db.prepare(
    `INSERT INTO sessions (id, label, user, started_at, ended_at, avg_hr, flagged_count, source) VALUES (?, ?, ?, ?, ?, ?, ?, 'seed')`
  );
  const insertReading = db.prepare(
    `INSERT INTO readings (session_id, ts, hr, perspiration, skin_temp, core_temp, respiration, elevated) VALUES (?, ?, ?, ?, ?, ?, ?, ?)`
  );

  const tx = db.transaction(() => {
    for (const d of DEMO) {
      const startedAt = now - d.startedAgoMin * 60000;
      const endedAt = startedAt + d.durationMin * 60000;
      const points = 9;
      let avgHr = 0;
      for (const m of METRICS) {
        const baseline = (m.normal[0] + m.normal[1]) / 2;
        const boost = d.intensity === "elevated" ? ELEVATED_BOOST[m.key] : 0;
        const series = genSeries(baseline, JITTER[m.key], boost, `${d.id}-${m.key}`, points);
        series.forEach((v, i) => {
          const ts = startedAt + Math.round((i / (points - 1)) * (endedAt - startedAt));
          if (m.key === "hr") avgHr += v;
          upsertPoint(ts, m.key, Number(v.toFixed(m.decimals)));
        });
      }
      insertSession.run(d.id, d.label, d.user, startedAt, endedAt, avgHr / points, d.flaggedCount);
      for (const [ts, vals] of Array.from(pointBuffer.entries()).sort((a, b) => a[0] - b[0])) {
        insertReading.run(d.id, ts, vals.hr ?? null, vals.perspiration ?? null, vals.skinTemp ?? null, vals.coreTemp ?? null, vals.respiration ?? null, 0);
      }
      pointBuffer.clear();
    }
  });
  tx();
}

const JITTER: Record<string, number> = { hr: 3, perspiration: 0.03, skinTemp: 0.15, coreTemp: 0.08, respiration: 1.2 };
const ELEVATED_BOOST: Record<string, number> = { hr: 26, perspiration: 0.22, skinTemp: 0.5, coreTemp: 0.35, respiration: 6 };

function hashStr(s: string): number {
  let h = 0;
  for (let i = 0; i < s.length; i++) h = (Math.imul(31, h) + s.charCodeAt(i)) | 0;
  return h;
}

function seededRandom(seed: number) {
  let s = seed | 0;
  return function () {
    s = (s + 0x6d2b79f5) | 0;
    let t = Math.imul(s ^ (s >>> 15), 1 | s);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function genSeries(baseline: number, jitter: number, boost: number, seedKey: string, points: number): number[] {
  const rand = seededRandom(hashStr(seedKey));
  const vals: number[] = [];
  const peakAt = Math.floor(points / 2);
  let v = baseline;
  for (let i = 0; i < points; i++) {
    const target = baseline + boost * Math.max(0, 1 - Math.abs(i - peakAt) / peakAt);
    v += (target - v) * 0.5 + (rand() - 0.5) * 2 * jitter;
    vals.push(v);
  }
  return vals;
}

const pointBuffer = new Map<number, Record<string, number>>();
function upsertPoint(ts: number, key: string, value: number) {
  const existing = pointBuffer.get(ts) ?? {};
  existing[key] = value;
  pointBuffer.set(ts, existing);
}

export default db;
