export type Point = { t: number; avg: number; min: number; max: number };

export const RANGES = [
  { label: "15 min", seconds: 15 * 60 },
  { label: "1 h", seconds: 3600 },
  { label: "6 h", seconds: 6 * 3600 },
  { label: "24 h", seconds: 24 * 3600 },
  { label: "7 d", seconds: 7 * 86400 },
  { label: "30 d", seconds: 30 * 86400 },
] as const;

/** Convert the API's point list to uPlot's column layout: [times, avg, min, max]. */
export function toColumns(points: Point[]): [number[], number[], number[], number[]] {
  return [
    points.map((p) => p.t),
    points.map((p) => p.avg),
    points.map((p) => p.min),
    points.map((p) => p.max),
  ];
}

export function formatBytes(n: number): string {
  const units = ["B", "KB", "MB", "GB", "TB"];
  let v = Math.abs(n);
  let i = 0;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i++;
  }
  return `${n < 0 ? "-" : ""}${v >= 100 || i === 0 ? v.toFixed(0) : v.toFixed(1)} ${units[i]}`;
}

export function formatValue(metric: string, v: number | undefined): string {
  if (v === undefined || !Number.isFinite(v)) return "–";
  if (metric.endsWith("_bytes") || metric.endsWith("_bytes_per_s")) {
    return formatBytes(v) + (metric.endsWith("_per_s") ? "/s" : "");
  }
  if (metric.endsWith("_pct") || metric.endsWith(".util") || metric.endsWith("util_total")) {
    return `${v.toFixed(1)}%`;
  }
  if (metric.endsWith("temp_c")) return `${v.toFixed(0)} °C`;
  if (metric.endsWith("power_w")) return `${v.toFixed(1)} W`;
  return v.toFixed(2);
}

/** A host is "up" when its last sample is no more than 60 s old (matches agent.down). */
export function isUp(lastSeen: number, nowSeconds: number): boolean {
  return nowSeconds - lastSeen <= 60;
}

export function ago(seconds: number): string {
  if (seconds < 90) return `${Math.max(0, Math.round(seconds))}s ago`;
  if (seconds < 5400) return `${Math.round(seconds / 60)}m ago`;
  return `${Math.round(seconds / 3600)}h ago`;
}

export type Series = { label: string; points: Point[] };

/** Align several series onto the union of their timestamps (gaps become null). */
export function mergeSeries(series: Series[]): (number | null)[][] {
  const times = [...new Set(series.flatMap((s) => s.points.map((p) => p.t)))].sort((a, b) => a - b);
  const cols: (number | null)[][] = [times];
  for (const s of series) {
    const byT = new Map(s.points.map((p) => [p.t, p.avg]));
    cols.push(times.map((t) => byT.get(t) ?? null));
  }
  return cols;
}
