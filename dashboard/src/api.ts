import type { Point } from "./lib/format";

export type Host = { host: string; last_seen: number };
export type Latest = { metric: string; device: string; value: number; ts: number };
export type MetricRef = { metric: string; device: string };
export type Rule = {
  id: number;
  name: string;
  host: string | null;
  metric: string;
  device: string | null;
  op: ">" | ">=" | "<" | "<=";
  threshold: number;
  for_seconds: number;
  severity: "info" | "warning" | "critical";
  notify_url: string | null;
  enabled: boolean;
};
export type RuleInput = Omit<Rule, "id">;
export type AlertEvent = {
  id: number;
  rule_id: number | null;
  rule_name: string | null;
  host: string;
  device: string;
  state: "firing" | "resolved";
  value: number | null;
  at: number;
};

const KEY_STORAGE = "meridian.apiKey";
export const getApiKey = () => localStorage.getItem(KEY_STORAGE) ?? "";
export const setApiKey = (k: string) => localStorage.setItem(KEY_STORAGE, k);

async function json<T>(res: Response): Promise<T> {
  if (!res.ok) {
    const detail = await res.text().catch(() => "");
    throw new Error(`${res.status} ${res.statusText}${detail ? `: ${detail}` : ""}`);
  }
  return res.json() as Promise<T>;
}

const writeHeaders = () => ({ "Content-Type": "application/json", "X-API-Key": getApiKey() });

export const api = {
  hosts: () => fetch("/api/hosts").then(json<Host[]>),
  metrics: (host: string) =>
    fetch(`/api/metrics?host=${encodeURIComponent(host)}`).then(json<MetricRef[]>),
  query: (host: string, metric: string, from: number, to: number, device?: string) => {
    const q = new URLSearchParams({ host, metric, from: String(from), to: String(to) });
    if (device) q.set("device", device);
    return fetch(`/api/query?${q}`).then(json<Point[]>);
  },
  alerts: () =>
    fetch("/api/alerts").then(json<{ firing: AlertEvent[]; history: AlertEvent[] }>),
  rules: () => fetch("/api/rules").then(json<Rule[]>),
  createRule: (r: RuleInput) =>
    fetch("/api/rules", { method: "POST", headers: writeHeaders(), body: JSON.stringify(r) }).then(
      json<Rule>,
    ),
  updateRule: (id: number, r: RuleInput) =>
    fetch(`/api/rules/${id}`, {
      method: "PUT",
      headers: writeHeaders(),
      body: JSON.stringify(r),
    }).then(json<Rule>),
  deleteRule: async (id: number) => {
    const res = await fetch(`/api/rules/${id}`, { method: "DELETE", headers: writeHeaders() });
    if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
  },
};
