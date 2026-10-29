import time

import pytest
from fastapi.testclient import TestClient

from app import db
from app.main import app
from app.security import RateLimiter

HDR = {"X-API-Key": "test-key"}


def body(ts=None, **kw):
    ts = time.time() if ts is None else ts
    return {
        "host": "h",
        "samples": [{"ts": ts, "metric": "cpu.util_total", "device": "total", "value": 1.5}],
        **kw,
    }


@pytest.fixture
def client(monkeypatch):
    monkeypatch.setattr(db, "insert_samples", lambda host, rows: len(rows))
    return TestClient(app)


def test_health_needs_no_auth(client):
    assert client.get("/health").json() == {"status": "ok"}


def test_ingest_ok(client):
    r = client.post("/ingest", json=body(), headers=HDR)
    assert r.status_code == 202 and r.json() == {"accepted": 1}


@pytest.mark.parametrize("headers", [{}, {"X-API-Key": "wrong"}, {"X-API-Key": ""}])
def test_bad_key_is_401(client, headers):
    assert client.post("/ingest", json=body(), headers=headers).status_code == 401


def test_missing_server_key_rejects_everything(client, monkeypatch):
    monkeypatch.delenv("API_KEY")
    assert client.post("/ingest", json=body(), headers={"X-API-Key": ""}).status_code == 401


@pytest.mark.parametrize(
    "payload",
    [
        {"host": "", "samples": []},
        {"host": "h", "samples": [{"ts": 1, "metric": "Bad Metric", "value": 1}]},
        {"host": "h", "samples": [{"ts": time.time(), "metric": "a.b", "value": "nan"}]},
        {"host": "h"},
    ],
)
def test_malformed_body_is_422(client, payload):
    assert client.post("/ingest", json=payload, headers=HDR).status_code == 422


def test_timestamps_outside_window_rejected(client):
    assert client.post("/ingest", json=body(time.time() + 400), headers=HDR).status_code == 422
    assert (
        client.post("/ingest", json=body(time.time() - 8 * 86400), headers=HDR).status_code == 422
    )
    assert client.post("/ingest", json=body(time.time() + 200), headers=HDR).status_code == 202


def test_oversized_body_is_413(client):
    r = client.post(
        "/ingest", content=b"x" * 1_000_001, headers={**HDR, "Content-Type": "application/json"}
    )
    assert r.status_code == 413


def test_rules_writes_require_key(client):
    rule = {"name": "n", "metric": "cpu.util_total", "op": ">", "threshold": 1}
    assert client.post("/api/rules", json=rule).status_code == 401
    assert client.put("/api/rules/1", json=rule).status_code == 401
    assert client.delete("/api/rules/1").status_code == 401


def test_rule_validation(client):
    bad = {"name": "n", "metric": "m", "op": "!=", "threshold": 1}
    assert client.post("/api/rules", json=bad, headers=HDR).status_code == 422
    bad = {
        "name": "n",
        "metric": "m",
        "op": ">",
        "threshold": 1,
        "notify_url": "file:///etc/passwd",
    }
    assert client.post("/api/rules", json=bad, headers=HDR).status_code == 422


def test_rate_limiter_window():
    rl = RateLimiter(2, window=10)
    assert rl.check("a", 0) and rl.check("a", 1)
    assert not rl.check("a", 2)
    assert rl.check("b", 2)
    assert rl.check("a", 11)


def test_source_selection_by_range():
    from datetime import timedelta as td

    assert db.pick_source(td(hours=6))[0] == "metrics"
    assert db.pick_source(td(hours=6, seconds=1))[0] == "metrics_1m"
    assert db.pick_source(td(days=7))[0] == "metrics_1m"
    assert db.pick_source(td(days=7, seconds=1))[0] == "metrics_1h"


def test_step_is_bounded():
    from datetime import timedelta as td

    assert db.pick_step(td(days=30), 5, 3600) == 3600  # never finer than the source
    assert db.pick_step(td(days=30), None, 3600) >= 3600
    assert db.pick_step(td(hours=6), 1, 0) == pytest.approx(10.8)  # <= 2000 points
