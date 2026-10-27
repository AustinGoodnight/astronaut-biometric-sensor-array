export function formatDateLabel(ts: number): string {
  const d = new Date(ts);
  const now = new Date();
  const startOfDay = (dt: Date) => new Date(dt.getFullYear(), dt.getMonth(), dt.getDate()).getTime();
  const diffDays = Math.round((startOfDay(now) - startOfDay(d)) / 86400000);
  if (diffDays <= 0) return "Today";
  if (diffDays === 1) return "Yesterday";
  if (diffDays < 7) return `${diffDays} days ago`;
  return d.toLocaleDateString([], { month: "short", day: "numeric" });
}

export function formatClock(ts: number): string {
  return new Date(ts).toLocaleTimeString([], { hour: "numeric", minute: "2-digit" });
}

export function formatTimeRange(start: number, end: number | null): string {
  if (!end) return `${formatClock(start)} – now`;
  return `${formatClock(start)} – ${formatClock(end)}`;
}

export function formatDuration(start: number, end: number | null): string {
  const endTs = end ?? Date.now();
  const totalMin = Math.max(1, Math.round((endTs - start) / 60000));
  if (totalMin < 60) return `${totalMin}m`;
  const h = Math.floor(totalMin / 60);
  const m = totalMin % 60;
  return m ? `${h}h ${m}m` : `${h}h`;
}
