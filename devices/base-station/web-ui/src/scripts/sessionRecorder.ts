// Client-side helper for the Start/Stop session control. The actual
// session + readings live in the SQLite database via the /api/sessions
// routes; localStorage here only remembers *which* session is currently
// being recorded (id + start time) so the UI can resume after a reload
// — it is not the data store.

const ACTIVE_KEY = "vitals:activeSession";

export interface ActiveSession {
  id: string;
  startedAt: number;
}

export function getActiveSession(): ActiveSession | null {
  try {
    const raw = localStorage.getItem(ACTIVE_KEY);
    return raw ? JSON.parse(raw) : null;
  } catch {
    return null;
  }
}

export async function startSession(label: string, user: string): Promise<ActiveSession> {
  const res = await fetch("/api/sessions", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ label, user }),
  });
  const data = await res.json();
  const session: ActiveSession = { id: data.id, startedAt: data.startedAt };
  try {
    localStorage.setItem(ACTIVE_KEY, JSON.stringify(session));
  } catch {}
  return session;
}

export async function stopSession(): Promise<void> {
  const active = getActiveSession();
  if (!active) return;
  try {
    localStorage.removeItem(ACTIVE_KEY);
  } catch {}
  await fetch(`/api/sessions/${active.id}/stop`, { method: "POST" }).catch(() => {});
}

export function formatElapsed(ms: number): string {
  const totalSec = Math.max(0, Math.floor(ms / 1000));
  const h = Math.floor(totalSec / 3600);
  const m = Math.floor((totalSec % 3600) / 60);
  const s = totalSec % 60;
  const pad = (n: number) => String(n).padStart(2, "0");
  return h > 0 ? `${h}:${pad(m)}:${pad(s)}` : `${pad(m)}:${pad(s)}`;
}
