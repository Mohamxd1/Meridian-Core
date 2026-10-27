"""Database access. All SQL is parameterized; user input never reaches an f-string
except through the fixed allow-list of table names chosen in `pick_source`."""

import os
from datetime import UTC, datetime, timedelta

from psycopg.rows import dict_row
from psycopg_pool import ConnectionPool

RAW_MAX = timedelta(hours=6)
M1_MAX = timedelta(days=7)
MAX_POINTS = 2000

_pool: ConnectionPool | None = None


def init_pool(url: str | None = None) -> ConnectionPool:
    global _pool
    url = url or os.environ["DATABASE_URL"]
    _pool = ConnectionPool(
        url, min_size=1, max_size=10, kwargs={"row_factory": dict_row}, open=True
    )
    return _pool


def pool() -> ConnectionPool:
    if _pool is None:
        raise RuntimeError("database pool is not initialized")
    return _pool


def close_pool() -> None:
    global _pool
    if _pool is not None:
        _pool.close()
        _pool = None


def pick_source(span: timedelta) -> tuple[str, float]:
    """Return (table, native resolution in seconds) for a requested time range."""
    if span <= RAW_MAX:
        return "metrics", 0.0
    if span <= M1_MAX:
        return "metrics_1m", 60.0
    return "metrics_1h", 3600.0


def pick_step(span: timedelta, requested: float | None, native: float) -> float:
    """Step in seconds: never finer than the source, never more than MAX_POINTS buckets."""
    floor = max(native, 1.0)
    auto = span.total_seconds() / MAX_POINTS
    step = requested if requested is not None else auto
    return max(step, auto, floor)


def insert_samples(host: str, samples: list[tuple[float, str, str, float]]) -> int:
    with (
        pool().connection() as conn,
        conn.cursor() as cur,
        cur.copy("COPY metrics (time, host, metric, device, value) FROM STDIN") as cp,
    ):
        for ts, metric, device, value in samples:
            cp.write_row((datetime.fromtimestamp(ts, UTC), host, metric, device, value))
    return len(samples)


def list_hosts() -> list[dict]:
    with pool().connection() as conn:
        return conn.execute(
            "SELECT host, max(time) AS last_seen FROM metrics GROUP BY host ORDER BY host"
        ).fetchall()


def list_metrics(host: str) -> list[dict]:
    with pool().connection() as conn:
        return conn.execute(
            "SELECT DISTINCT metric, device FROM metrics WHERE host = %s ORDER BY metric, device",
            (host,),
        ).fetchall()


def query_series(
    host: str, metric: str, device: str | None, start: datetime, end: datetime, step: float | None
) -> tuple[str, float, list[dict]]:
    table, native = pick_source(end - start)
    step = pick_step(end - start, step, native)
    if table == "metrics":
        agg = "avg(value), min(value), max(value)"
        time_col = "time"
    else:
        agg = "avg(avg), min(min), max(max)"
        time_col = "bucket"
    # `table` and `time_col` come from the fixed branches above, never from the request.
    sql = (
        f"SELECT time_bucket(make_interval(secs => %s), {time_col}) AS t, {agg} "
        f"FROM {table} WHERE host = %s AND metric = %s "
        f"AND (%s::text IS NULL OR device = %s) AND {time_col} >= %s AND {time_col} < %s "
        "GROUP BY t ORDER BY t"
    )
    with pool().connection() as conn:
        rows = conn.execute(sql, (step, host, metric, device, device, start, end)).fetchall()
    points = [
        {"t": r["t"].timestamp(), "avg": r["avg"], "min": r["min"], "max": r["max"]} for r in rows
    ]
    return table, step, points


def latest_values(host: str, within: timedelta = timedelta(minutes=2)) -> list[dict]:
    with pool().connection() as conn:
        return conn.execute(
            "SELECT DISTINCT ON (metric, device) metric, device, value, time "
            "FROM metrics WHERE host = %s AND time > %s "
            "ORDER BY metric, device, time DESC",
            (host, datetime.now(UTC) - within),
        ).fetchall()


_RULE_COLS = (
    "id, name, host, metric, device, op, threshold, for_seconds, severity, notify_url, enabled"
)


def list_rules() -> list[dict]:
    with pool().connection() as conn:
        return conn.execute(f"SELECT {_RULE_COLS} FROM alert_rules ORDER BY id").fetchall()


def get_rule(rule_id: int) -> dict | None:
    with pool().connection() as conn:
        return conn.execute(
            f"SELECT {_RULE_COLS} FROM alert_rules WHERE id = %s", (rule_id,)
        ).fetchone()


_FIELDS = (
    "name",
    "host",
    "metric",
    "device",
    "op",
    "threshold",
    "for_seconds",
    "severity",
    "notify_url",
    "enabled",
)


def create_rule(data: dict) -> dict:
    with pool().connection() as conn:
        return conn.execute(
            f"INSERT INTO alert_rules ({', '.join(_FIELDS)}) "
            f"VALUES ({', '.join(['%s'] * len(_FIELDS))}) RETURNING {_RULE_COLS}",
            tuple(data[f] for f in _FIELDS),
        ).fetchone()


def update_rule(rule_id: int, data: dict) -> dict | None:
    assignments = ", ".join(f"{f} = %s" for f in _FIELDS)  # field names are constants
    with pool().connection() as conn:
        return conn.execute(
            f"UPDATE alert_rules SET {assignments} WHERE id = %s RETURNING {_RULE_COLS}",
            (*(data[f] for f in _FIELDS), rule_id),
        ).fetchone()


def delete_rule(rule_id: int) -> bool:
    with pool().connection() as conn:
        return conn.execute("DELETE FROM alert_rules WHERE id = %s", (rule_id,)).rowcount > 0


def alert_history(limit: int) -> list[dict]:
    with pool().connection() as conn:
        return conn.execute(
            "SELECT e.id, e.rule_id, r.name AS rule_name, e.host, e.device, e.state, e.value, e.at "
            "FROM alert_events e LEFT JOIN alert_rules r ON r.id = e.rule_id "
            "ORDER BY e.at DESC, e.id DESC LIMIT %s",
            (limit,),
        ).fetchall()


def firing_alerts() -> list[dict]:
    with pool().connection() as conn:
        return conn.execute(
            "SELECT * FROM ("
            " SELECT DISTINCT ON (e.rule_id, e.host, e.device)"
            "  e.id, e.rule_id, r.name AS rule_name, e.host, e.device, e.state, e.value, e.at"
            " FROM alert_events e LEFT JOIN alert_rules r ON r.id = e.rule_id"
            " ORDER BY e.rule_id, e.host, e.device, e.at DESC, e.id DESC"
            ") latest WHERE state = 'firing' ORDER BY at DESC"
        ).fetchall()
