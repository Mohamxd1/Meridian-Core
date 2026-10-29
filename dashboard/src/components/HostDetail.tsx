import { useEffect, useState } from "react";
import { api } from "../api";
import { panelsFor, type Panel } from "../lib/panels";
import { RANGES, type Series } from "../lib/format";
import { Chart } from "./Chart";

const label = (d: string) => d || "all";
const byDevice = (a: string, b: string) => a.localeCompare(b, undefined, { numeric: true });

function PanelChart({ host, panel, rangeS, tick }: { host: string; panel: Panel; rangeS: number; tick: number }) {
  const [series, setSeries] = useState<Series[]>([]);
  const [error, setError] = useState("");
  const devices = panel.devices.join("\u0000");
  useEffect(() => {
    const to = Math.floor(Date.now() / 1000);
    Promise.all(
      [...panel.devices].sort(byDevice).map(async (d) => ({
        label: label(d),
        points: await api.query(host, panel.metric, to - rangeS, to, d || undefined),
      })),
    )
      .then((s) => { setSeries(s); setError(""); })
      .catch((e) => setError(String(e)));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [host, panel.metric, devices, rangeS, tick]);
  return (
    <>
      <Chart title={panel.title} series={series} unit={panel.unit} max={panel.max} />
      {error && <p className="error">{error}</p>}
    </>
  );
}

export function HostDetail({ host, onBack }: { host: string; onBack: () => void }) {
  const [range, setRange] = useState<number>(RANGES[1].seconds);
  const [panels, setPanels] = useState<Panel[]>([]);
  const [tick, setTick] = useState(0);

  useEffect(() => {
    api.metrics(host).then((m) => setPanels(panelsFor(m))).catch(() => setPanels([]));
  }, [host]);

  useEffect(() => {
    const id = setInterval(() => setTick((t) => t + 1), Math.max(5000, (range / 2000) * 1000));
    return () => clearInterval(id);
  }, [range]);

  return (
    <div>
      <div className="toolbar">
        <button onClick={onBack}>← Hosts</button>
        <h2>{host}</h2>
        <div className="ranges" role="group" aria-label="time range">
          {RANGES.map((r) => (
            <button key={r.label} className={r.seconds === range ? "active" : ""} onClick={() => setRange(r.seconds)}>
              {r.label}
            </button>
          ))}
        </div>
      </div>
      <div className="charts">
        {panels.map((p) => (
          <PanelChart key={p.metric} host={host} panel={p} rangeS={range} tick={tick} />
        ))}
        {panels.length === 0 && <p className="empty">No metrics for this host yet.</p>}
      </div>
    </div>
  );
}
