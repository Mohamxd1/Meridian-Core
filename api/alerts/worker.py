"""Alert evaluation loop: python -m alerts.worker"""

import logging
import os
import time
from collections.abc import Callable
from datetime import UTC, datetime

import psycopg
from psycopg.rows import dict_row

from . import notifiers
from .rules import (
    FIRING,
    OK,
    Rule,
    evaluate_window,
    hosts_down,
    should_repeat,
    transition,
)

log = logging.getLogger("alerts.worker")
INTERVAL_S = 15
AGENT_DOWN_RULE = "Agent down"
AGENT_DOWN_METRIC = "agent.down"

Key = tuple[int, str, str]  # (rule_id, host, device)
Notify = Callable[[str, str | None], None]


class State:
    def __init__(self) -> None:
        self.states: dict[Key, str] = {}
        self.last_notified: dict[Key, float] = {}


def ensure_agent_down_rule(conn) -> int:
    row = conn.execute(
        "SELECT id FROM alert_rules WHERE metric = %s LIMIT 1", (AGENT_DOWN_METRIC,)
    ).fetchone()
    if row:
        return row["id"]
    return conn.execute(
        "INSERT INTO alert_rules (name, metric, op, threshold, for_seconds, severity) "
        "VALUES (%s, %s, '>', 60, 60, 'critical') RETURNING id",
        (AGENT_DOWN_RULE, AGENT_DOWN_METRIC),
    ).fetchone()["id"]


def restore_state(conn, state: State, now: float) -> None:
    rows = conn.execute(
        "SELECT * FROM (SELECT DISTINCT ON (rule_id, host, device) rule_id, host, device, state "
        "FROM alert_events ORDER BY rule_id, host, device, at DESC, id DESC) t "
        "WHERE state = 'firing'"
    ).fetchall()
    for r in rows:
        key = (r["rule_id"], r["host"], r["device"])
        state.states[key] = FIRING
        state.last_notified[key] = now


def record(conn, key: Key, event: str, value: float | None) -> None:
    conn.execute(
        "INSERT INTO alert_events (rule_id, host, device, state, value) VALUES (%s,%s,%s,%s,%s)",
        (*key, event, value),
    )


def apply_verdict(
    conn,
    state: State,
    rule_name: str,
    rule: Rule | None,
    key: Key,
    verdict: str,
    value: float | None,
    now: float,
    notify: Notify,
    metric: str,
    severity: str,
    url: str | None,
) -> None:
    prev = state.states.get(key, OK)
    new, event = transition(prev, verdict)
    state.states[key] = new
    repeat = (
        event is None and new == FIRING and should_repeat(state.last_notified.get(key, now), now)
    )
    if event is None and not repeat:
        return
    if event is not None:
        record(conn, key, event, value)
    state.last_notified[key] = now
    notify(
        notifiers.format_message(event or "firing", rule_name, key[1], metric, value, severity), url
    )


def load_rules(conn) -> list[Rule]:
    rows = conn.execute(
        "SELECT id, name, metric, op, threshold, for_seconds, host, device, severity, notify_url "
        "FROM alert_rules WHERE enabled AND metric <> %s",
        (AGENT_DOWN_METRIC,),
    ).fetchall()
    return [Rule(**r) for r in rows]


def run_cycle(conn, state: State, now: float, notify: Notify = notifiers.notify) -> None:
    since = datetime.fromtimestamp(now, UTC)
    for rule in load_rules(conn):
        start = datetime.fromtimestamp(now - rule.for_seconds, UTC)
        rows = conn.execute(
            "SELECT host, device, extract(epoch FROM time) AS ts, value FROM metrics "
            "WHERE metric = %s AND time >= %s AND time <= %s "
            "AND (%s::text IS NULL OR host = %s) AND (%s::text IS NULL OR device = %s) "
            "ORDER BY time",
            (rule.metric, start, since, rule.host, rule.host, rule.device, rule.device),
        ).fetchall()
        groups: dict[tuple[str, str], list[tuple[float, float]]] = {}
        for r in rows:
            groups.setdefault((r["host"], r["device"]), []).append((float(r["ts"]), r["value"]))
        for (host, device), samples in groups.items():
            verdict = evaluate_window(rule, samples, now)
            apply_verdict(
                conn,
                state,
                rule.name,
                rule,
                (rule.id, host, device),
                verdict,
                samples[-1][1],
                now,
                notify,
                rule.metric,
                rule.severity,
                rule.notify_url,
            )

    # No-data detection: a host silent for more than 60 s fires agent.down.
    down_id = ensure_agent_down_rule(conn)
    last_seen = {
        r["host"]: float(r["ts"])
        for r in conn.execute(
            "SELECT host, extract(epoch FROM max(time)) AS ts FROM metrics "
            "WHERE time > %s GROUP BY host",
            (datetime.fromtimestamp(now - 3600, UTC),),
        ).fetchall()
    }
    down = hosts_down(last_seen, now)
    for host in last_seen:
        verdict = "breach" if host in down else "healthy"
        apply_verdict(
            conn,
            state,
            AGENT_DOWN_RULE,
            None,
            (down_id, host, ""),
            verdict,
            None,
            now,
            notify,
            AGENT_DOWN_METRIC,
            "critical",
            None,
        )
    # Hosts that left the one-hour lookback entirely stay firing until they return.


def main() -> None:
    logging.basicConfig(level=logging.INFO)
    state = State()
    restored = False
    while True:
        try:
            with psycopg.connect(
                os.environ["DATABASE_URL"], row_factory=dict_row, autocommit=True
            ) as conn:
                if not restored:
                    restore_state(conn, state, time.time())
                    restored = True
                while True:
                    run_cycle(conn, state, time.time())
                    time.sleep(INTERVAL_S)
        except psycopg.Error:
            log.exception("database error; reconnecting in %ss", INTERVAL_S)
            time.sleep(INTERVAL_S)


if __name__ == "__main__":
    main()
