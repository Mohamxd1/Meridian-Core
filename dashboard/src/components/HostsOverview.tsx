import { useEffect, useState } from "react";
import { api, type Host, type Latest } from "../api";
import { ago, formatValue, isUp } from "../lib/format";

type Props = { onOpen: (host: string) => void };

const CARDS: [string, string, string][] = [
  ["CPU", "cpu.util_total", "total"],
  ["Memory", "mem.used_pct", ""],
  ["GPU", "gpu.util", "gpu0"],
  ["GPU temp", "gpu.temp_c", "gpu0"],
];

function HostCard({ host, onOpen }: { host: Host; onOpen: (h: string) => void }) {
  const [latest, setLatest] = useState<Latest[]>([]);
  const [now, setNow] = useState(Date.now() / 1000);

  useEffect(() => {
    const es = new EventSource(`/api/stream?host=${encodeURIComponent(host.host)}`);
    es.addEventListener("latest", (e) => setLatest(JSON.parse((e as MessageEvent).data)));
    const tick = setInterval(() => setNow(Date.now() / 1000), 1000);
    return () => {
      es.close();
      clearInterval(tick);
    };
  }, [host.host]);

  const newest = Math.max(host.last_seen, ...latest.map((l) => l.ts));
  const up = isUp(newest, now);
  return (
    <button className="card" onClick={() => onOpen(host.host)}>
      <div className="card-head">
        <strong>{host.host}</strong>
        <span className={up ? "badge up" : "badge down"}>{up ? "agent up" : "agent down"}</span>
      </div>
      <div className="card-sub">last seen {ago(now - newest)}</div>
      <div className="stats">
        {CARDS.map(([label, metric, device]) => {
          const v = latest.find((l) => l.metric === metric && l.device === device)?.value;
          return (
            <div key={metric}>
              <div className="stat-label">{label}</div>
              <div className="stat-value">{formatValue(metric, v)}</div>
            </div>
          );
        })}
      </div>
    </button>
  );
}

export function HostsOverview({ onOpen }: Props) {
  const [hosts, setHosts] = useState<Host[] | null>(null);
  const [error, setError] = useState("");

  useEffect(() => {
    const load = () =>
      api.hosts().then((h) => { setHosts(h); setError(""); }).catch((e) => setError(String(e)));
    load();
    const id = setInterval(load, 10000);
    return () => clearInterval(id);
  }, []);

  if (error) return <p className="error">Could not load hosts: {error}</p>;
  if (!hosts) return <p className="empty">loading…</p>;
  if (hosts.length === 0) return <p className="empty">No hosts yet. Start an agent to see data here.</p>;
  return (
    <div className="grid">
      {hosts.map((h) => (
        <HostCard key={h.host} host={h} onOpen={onOpen} />
      ))}
    </div>
  );
}
