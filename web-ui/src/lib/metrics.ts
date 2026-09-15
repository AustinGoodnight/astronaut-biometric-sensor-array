export interface MetricDef {
  key: string;
  label: string;
  unit: string;
  color: string;
  normal: [number, number];
  decimals: number;
  icon: string;
}

export const METRICS: MetricDef[] = [
  { key: "hr", label: "Heart Rate", unit: " bpm", color: "oklch(56% 0.20 15)", normal: [60, 92], decimals: 0, icon: "heart" },
  { key: "perspiration", label: "Perspiration", unit: " mg/min", color: "oklch(55% 0.13 235)", normal: [0.2, 0.7], decimals: 2, icon: "droplet" },
  { key: "skinTemp", label: "Skin Temperature", unit: " °C", color: "oklch(58% 0.13 165)", normal: [32, 34.2], decimals: 1, icon: "therm" },
  { key: "coreTemp", label: "Core Temperature", unit: " °C", color: "oklch(62% 0.16 70)", normal: [36.8, 38.2], decimals: 1, icon: "flame" },
  { key: "respiration", label: "Respiration", unit: " br/min", color: "oklch(53% 0.15 300)", normal: [12, 20], decimals: 0, icon: "lungs" },
];

export function metric(key: string): MetricDef {
  const m = METRICS.find((m) => m.key === key);
  if (!m) throw new Error(`Unknown metric: ${key}`);
  return m;
}

export function normalRangeText(key: string): string {
  const m = metric(key);
  return `${m.normal[0].toFixed(m.decimals)}–${m.normal[1].toFixed(m.decimals)}${m.unit}`;
}
