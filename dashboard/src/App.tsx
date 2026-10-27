import { useState } from "react";
import { Alerts } from "./components/Alerts";
import { HostDetail } from "./components/HostDetail";
import { HostsOverview } from "./components/HostsOverview";

type Tab = "hosts" | "alerts";

export default function App() {
  const [tab, setTab] = useState<Tab>("hosts");
  const [host, setHost] = useState<string | null>(null);
  return (
    <>
      <header>
        <strong className="brand">Meridian Core</strong>
        <nav>
          <button className={tab === "hosts" ? "active" : ""} onClick={() => { setTab("hosts"); setHost(null); }}>Hosts</button>
          <button className={tab === "alerts" ? "active" : ""} onClick={() => setTab("alerts")}>Alerts</button>
        </nav>
      </header>
      <main>
        {tab === "alerts" ? <Alerts /> : host ? (
          <HostDetail host={host} onBack={() => setHost(null)} />
        ) : (
          <HostsOverview onOpen={setHost} />
        )}
      </main>
    </>
  );
}
