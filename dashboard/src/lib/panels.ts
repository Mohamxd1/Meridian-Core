import type { MetricRef } from "../api";

export type Spec = { title: string; metric: string; unit?: string; max?: number; device?: string };

const SPECS: Spec[] = [
  { title: "CPU total", metric: "cpu.util_total", device: "total", unit: "%", max: 100 },
  { title: "CPU per core", metric: "cpu.util_core", unit: "%", max: 100 },
  { title: "Memory used", metric: "mem.used_pct", unit: "%", max: 100 },
  { title: "GPU utilization", metric: "gpu.util", unit: "%", max: 100 },
  { title: "GPU memory used (bytes)", metric: "gpu.mem_used_bytes" },
  { title: "GPU temperature", metric: "gpu.temp_c", unit: "°C" },
  { title: "GPU power", metric: "gpu.power_w", unit: "W" },
  { title: "Network receive (bytes/s)", metric: "net.rx_bytes_per_s" },
  { title: "Network transmit (bytes/s)", metric: "net.tx_bytes_per_s" },
  { title: "Disk used", metric: "disk.used_pct", unit: "%", max: 100 },
];

export type Panel = Spec & { devices: string[] };

/** One panel per known metric; one series per device (e.g. each core, GPU, interface, mount). */
export function panelsFor(available: MetricRef[]): Panel[] {
  const out: Panel[] = [];
  for (const s of SPECS) {
    const all = available.filter((m) => m.metric === s.metric).map((m) => m.device);
    const devices = s.device ? all.filter((d) => d === s.device) : all;
    if (devices.length > 0) out.push({ ...s, devices });
  }
  return out;
}
