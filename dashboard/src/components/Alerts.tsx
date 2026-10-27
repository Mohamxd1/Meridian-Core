import { useCallback, useEffect, useState } from "react";
import { api, getApiKey, setApiKey, type AlertEvent, type Rule, type RuleInput } from "../api";

const blank: RuleInput = {
  name: "", host: null, metric: "cpu.util_total", device: null, op: ">", threshold: 90,
  for_seconds: 300, severity: "warning", notify_url: null, enabled: true,
};

const when = (t: number) => new Date(t * 1000).toLocaleString();

function RuleEditor({ rule, onSaved, onCancel }: { rule: Rule | null; onSaved: () => void; onCancel: () => void }) {
  const [draft, setDraft] = useState<RuleInput>(rule ? { ...rule } : blank);
  const [error, setError] = useState("");
  const set = <K extends keyof RuleInput>(k: K, v: RuleInput[K]) => setDraft((d) => ({ ...d, [k]: v }));
  const blankToNull = (v: string) => (v.trim() === "" ? null : v.trim());

  const save = async (e: React.FormEvent) => {
    e.preventDefault();
    try {
      if (rule) await api.updateRule(rule.id, draft);
      else await api.createRule(draft);
      onSaved();
    } catch (err) {
      setError(String(err));
    }
  };

  return (
    <form className="panel editor" onSubmit={save}>
      <h3>{rule ? `Edit rule #${rule.id}` : "New rule"}</h3>
      <label>Name<input required value={draft.name} onChange={(e) => set("name", e.target.value)} /></label>
      <label>Metric<input required pattern="[a-z0-9_.]+" value={draft.metric} onChange={(e) => set("metric", e.target.value)} /></label>
      <label>Host (blank = all)<input value={draft.host ?? ""} onChange={(e) => set("host", blankToNull(e.target.value))} /></label>
      <label>Device (blank = any)<input value={draft.device ?? ""} onChange={(e) => set("device", blankToNull(e.target.value))} /></label>
      <div className="row">
        <label>Condition
          <select value={draft.op} onChange={(e) => set("op", e.target.value as Rule["op"])}>
            {[">", ">=", "<", "<="].map((o) => <option key={o}>{o}</option>)}
          </select>
        </label>
        <label>Threshold<input type="number" step="any" required value={draft.threshold} onChange={(e) => set("threshold", Number(e.target.value))} /></label>
        <label>For (seconds)<input type="number" min={0} required value={draft.for_seconds} onChange={(e) => set("for_seconds", Number(e.target.value))} /></label>
        <label>Severity
          <select value={draft.severity} onChange={(e) => set("severity", e.target.value as Rule["severity"])}>
            {["info", "warning", "critical"].map((o) => <option key={o}>{o}</option>)}
          </select>
        </label>
      </div>
      <label>Webhook URL (blank = server default)<input type="url" value={draft.notify_url ?? ""} onChange={(e) => set("notify_url", blankToNull(e.target.value))} /></label>
      <label className="check"><input type="checkbox" checked={draft.enabled} onChange={(e) => set("enabled", e.target.checked)} /> Enabled</label>
      {error && <p className="error">{error}</p>}
      <div className="row"><button type="submit" className="primary">Save</button><button type="button" onClick={onCancel}>Cancel</button></div>
    </form>
  );
}

function EventTable({ events }: { events: AlertEvent[] }) {
  if (events.length === 0) return <p className="empty">none</p>;
  return (
    <table>
      <thead><tr><th>Time</th><th>Rule</th><th>Host</th><th>Device</th><th>State</th><th>Value</th></tr></thead>
      <tbody>
        {events.map((e) => (
          <tr key={e.id}>
            <td>{when(e.at)}</td><td>{e.rule_name ?? e.rule_id}</td><td>{e.host}</td><td>{e.device}</td>
            <td><span className={e.state === "firing" ? "badge down" : "badge up"}>{e.state}</span></td>
            <td>{e.value === null ? "–" : e.value.toFixed(1)}</td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}

export function Alerts() {
  const [firing, setFiring] = useState<AlertEvent[]>([]);
  const [history, setHistory] = useState<AlertEvent[]>([]);
  const [rules, setRules] = useState<Rule[]>([]);
  const [editing, setEditing] = useState<Rule | "new" | null>(null);
  const [key, setKey] = useState(getApiKey());
  const [error, setError] = useState("");

  const load = useCallback(() => {
    Promise.all([api.alerts(), api.rules()])
      .then(([a, r]) => { setFiring(a.firing); setHistory(a.history); setRules(r); setError(""); })
      .catch((e) => setError(String(e)));
  }, []);
  useEffect(() => {
    load();
    const id = setInterval(load, 15000);
    return () => clearInterval(id);
  }, [load]);

  const remove = async (r: Rule) => {
    if (!confirm(`Delete rule "${r.name}"?`)) return;
    try { await api.deleteRule(r.id); load(); } catch (e) { setError(String(e)); }
  };

  return (
    <div>
      {error && <p className="error">{error}</p>}
      <section className="panel">
        <h3>Firing now</h3>
        {firing.length === 0 ? <p className="empty">Nothing is firing.</p> : <EventTable events={firing} />}
      </section>
      <section className="panel">
        <div className="toolbar">
          <h3>Rules</h3>
          <label className="inline">API key (for edits)
            <input type="password" value={key} onChange={(e) => { setKey(e.target.value); setApiKey(e.target.value); }} />
          </label>
          <button className="primary" onClick={() => setEditing("new")}>Add rule</button>
        </div>
        <table>
          <thead><tr><th>Name</th><th>Condition</th><th>Scope</th><th>For</th><th>Severity</th><th>On</th><th /></tr></thead>
          <tbody>
            {rules.map((r) => (
              <tr key={r.id}>
                <td>{r.name}</td><td>{r.metric} {r.op} {r.threshold}</td>
                <td>{r.host ?? "all hosts"}{r.device ? ` / ${r.device}` : ""}</td>
                <td>{r.for_seconds}s</td><td>{r.severity}</td><td>{r.enabled ? "yes" : "no"}</td>
                <td><button onClick={() => setEditing(r)}>Edit</button> <button onClick={() => remove(r)}>Delete</button></td>
              </tr>
            ))}
          </tbody>
        </table>
      </section>
      {editing && (
        <RuleEditor rule={editing === "new" ? null : editing} onCancel={() => setEditing(null)}
          onSaved={() => { setEditing(null); load(); }} />
      )}
      <section className="panel"><h3>History</h3><EventTable events={history} /></section>
    </div>
  );
}
