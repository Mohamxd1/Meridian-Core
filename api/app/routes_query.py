import time
from datetime import UTC, datetime

from fastapi import APIRouter, Query, Response

from . import db

router = APIRouter(prefix="/api")


def _ts(value: float | None, default: float) -> datetime:
    return datetime.fromtimestamp(default if value is None else value, UTC)


@router.get("/hosts")
def hosts() -> list[dict]:
    return [{"host": r["host"], "last_seen": r["last_seen"].timestamp()} for r in db.list_hosts()]


@router.get("/metrics")
def metrics(host: str = Query(min_length=1)) -> list[dict]:
    return db.list_metrics(host)


@router.get("/query")
def query(
    response: Response,
    host: str = Query(min_length=1),
    metric: str = Query(pattern=r"^[a-z0-9_.]+$"),
    device: str | None = None,
    from_: float | None = Query(default=None, alias="from", description="Unix seconds"),
    to: float | None = Query(default=None, description="Unix seconds"),
    step: float | None = Query(default=None, gt=0, description="Bucket size in seconds"),
) -> list[dict]:
    now = time.time()
    end = _ts(to, now)
    start = _ts(from_, now - 3600)
    if start >= end:
        from fastapi import HTTPException

        raise HTTPException(422, "'from' must be before 'to'")
    table, used_step, points = db.query_series(host, metric, device, start, end, step)
    response.headers["X-Source-Table"] = table
    response.headers["X-Step-Seconds"] = f"{used_step:g}"
    return points


@router.get("/alerts")
def alerts(limit: int = Query(default=100, ge=1, le=1000)) -> dict:
    def fmt(r: dict) -> dict:
        return {**r, "at": r["at"].timestamp()}

    return {
        "firing": [fmt(r) for r in db.firing_alerts()],
        "history": [fmt(r) for r in db.alert_history(limit)],
    }
