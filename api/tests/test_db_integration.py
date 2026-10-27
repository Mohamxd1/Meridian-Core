"""Run against real PostgreSQL: TEST_DATABASE_URL=postgresql://... pytest"""

import time

from conftest import refresh_rollups
from fastapi.testclient import TestClient

from alerts import worker
from app import db
from app.main import app

HDR = {"X-API-Key": "test-key"}


def put(pg, host, metric, values, end, step=5.0, device="total"):
    n = len(values)
    for i, v in enumerate(values):
        pg.execute(
            "INSERT INTO metrics VALUES (to_timestamp(%s), %s, %s, %s, %s)",
            (end - (n - 1 - i) * step, host, metric, device, v),
        )


def test_ingest_then_query_and_hosts(pg):
    c = TestClient(app)
    now = time.time()
    batch = {
        "host": "pc",
        "samples": [
            {"ts": now - 10, "metric": "cpu.util_total", "device": "total", "value": 10},
            {"ts": now - 5, "metric": "cpu.util_total", "device": "total", "value": 30},
            {"ts": now, "metric": "gpu.util", "device": "gpu0", "value": 70},
        ],
    }
    assert c.post("/ingest", json=batch, headers=HDR).json() == {"accepted": 3}
    assert pg.execute("SELECT count(*) AS n FROM metrics").fetchone()["n"] == 3

    hosts = c.get("/api/hosts").json()
    assert [h["host"] for h in hosts] == ["pc"]
    metrics = c.get("/api/metrics", params={"host": "pc"}).json()
    assert {"metric": "gpu.util", "device": "gpu0"} in metrics

    r = c.get(
        "/api/query",
        params={
            "host": "pc",
            "metric": "cpu.util_total",
            "from": now - 60,
            "to": now + 1,
            "step": 60,
        },
    )
    assert r.headers["x-source-table"] == "metrics"
    assert sum(p["max"] for p in r.json()) >= 30
    assert r.json()[0]["min"] <= r.json()[0]["avg"] <= r.json()[0]["max"]


def test_query_empty_and_rollup_sources(pg):
    c = TestClient(app)
    now = time.time()
    r = c.get("/api/query", params={"host": "none", "metric": "cpu.util_total"})
    assert r.status_code == 200 and r.json() == []
    put(pg, "pc", "cpu.util_total", [10, 20, 30, 40], now - 3 * 86400, step=60)
    refresh_rollups(pg)
    r = c.get(
        "/api/query",
        params={"host": "pc", "metric": "cpu.util_total", "from": now - 4 * 86400, "to": now},
    )
    assert r.headers["x-source-table"] == "metrics_1m" and len(r.json()) >= 1
    r = c.get(
        "/api/query",
        params={"host": "pc", "metric": "cpu.util_total", "from": now - 30 * 86400, "to": now},
    )
    assert r.headers["x-source-table"] == "metrics_1h"
    assert (
        c.get("/api/query", params={"host": "pc", "metric": "x", "from": 5, "to": 1}).status_code
        == 422
    )


def test_rule_crud(pg):
    c = TestClient(app)
    rule = {"name": "hot", "metric": "gpu.temp_c", "op": ">", "threshold": 85, "for_seconds": 120}
    created = c.post("/api/rules", json=rule, headers=HDR)
    assert created.status_code == 201
    rid = created.json()["id"]
    rule["threshold"] = 90
    assert c.put(f"/api/rules/{rid}", json=rule, headers=HDR).json()["threshold"] == 90
    assert [r["id"] for r in c.get("/api/rules").json()] == [rid]
    assert c.delete(f"/api/rules/{rid}", headers=HDR).status_code == 204
    assert c.delete(f"/api/rules/{rid}", headers=HDR).status_code == 404


def test_worker_fires_once_resolves_once_and_agent_down(pg):
    pg.execute(
        "INSERT INTO alert_rules (name, metric, op, threshold, for_seconds) "
        "VALUES ('CPU', 'cpu.util_total', '>', 90, 60)"
    )
    state, sent = worker.State(), []
    notify = lambda text, url: sent.append(text)
    now = time.time()
    put(pg, "pc", "cpu.util_total", [95] * 13, now)
    for dt in (0, 15, 30):  # repeated cycles must not re-notify
        worker.run_cycle(pg, state, now + dt, notify)
    assert len(sent) == 1 and "CPU" in sent[0]
    put(pg, "pc", "cpu.util_total", [10] * 13, now + 120)
    worker.run_cycle(pg, state, now + 120, notify)
    worker.run_cycle(pg, state, now + 135, notify)
    states = [
        r["state"]
        for r in pg.execute(
            "SELECT state FROM alert_events WHERE host='pc' AND rule_id IN "
            "(SELECT id FROM alert_rules WHERE metric='cpu.util_total') ORDER BY id"
        ).fetchall()
    ]
    assert states == ["firing", "resolved"]
    assert len(sent) == 2

    # silence: more than 60 s after the last sample -> agent.down fires once
    worker.run_cycle(pg, state, now + 120 + 90, notify)
    worker.run_cycle(pg, state, now + 120 + 105, notify)
    down = [
        r
        for r in pg.execute(
            "SELECT e.state FROM alert_events e JOIN alert_rules r "
            "ON r.id=e.rule_id WHERE r.metric='agent.down'"
        ).fetchall()
    ]
    assert [d["state"] for d in down] == ["firing"]

    firing = TestClient(app).get("/api/alerts").json()
    assert firing["history"] and [f["host"] for f in firing["firing"]] == ["pc"]


def test_restore_state_prevents_duplicate_firing_after_restart(pg):
    rid = pg.execute(
        "INSERT INTO alert_rules (name, metric, op, threshold, for_seconds) "
        "VALUES ('CPU','cpu.util_total','>',90,60) RETURNING id"
    ).fetchone()["id"]
    pg.execute(
        "INSERT INTO alert_events (rule_id, host, device, state) VALUES (%s,'pc','total','firing')",
        (rid,),
    )
    state = worker.State()
    worker.restore_state(pg, state, time.time())
    sent = []
    now = time.time()
    put(pg, "pc", "cpu.util_total", [99] * 13, now)
    worker.run_cycle(pg, state, now, lambda t, u: sent.append(t))
    assert sent == []
    assert db.firing_alerts()
