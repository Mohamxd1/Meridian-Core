import asyncio
import json

from fastapi import APIRouter, Query
from sse_starlette.sse import EventSourceResponse

from . import db

router = APIRouter(prefix="/api")


def encode_latest(rows: list[dict]) -> str:
    return json.dumps(
        [
            {
                "metric": r["metric"],
                "device": r["device"],
                "value": r["value"],
                "ts": r["time"].timestamp(),
            }
            for r in rows
        ]
    )


@router.get("/stream")
async def stream(
    host: str = Query(min_length=1), interval: float = Query(default=1.0, ge=0.2, le=30)
):
    async def events():
        while True:
            rows = await asyncio.to_thread(db.latest_values, host)
            yield {"event": "latest", "data": encode_latest(rows)}
            await asyncio.sleep(interval)

    return EventSourceResponse(events())
