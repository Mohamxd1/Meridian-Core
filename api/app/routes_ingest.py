import time

from fastapi import APIRouter, Depends, HTTPException, Request

from . import db
from .schemas import IngestBatch, IngestResponse
from .security import limit_rate, require_api_key

router = APIRouter()

MAX_FUTURE_S = 5 * 60
MAX_PAST_S = 7 * 24 * 3600


def check_timestamps(samples, now: float) -> None:
    for s in samples:
        if s.ts > now + MAX_FUTURE_S:
            raise HTTPException(422, f"timestamp {s.ts} is more than 5 minutes in the future")
        if s.ts < now - MAX_PAST_S:
            raise HTTPException(422, f"timestamp {s.ts} is more than 7 days in the past")


@router.post(
    "/ingest",
    status_code=202,
    response_model=IngestResponse,
    dependencies=[Depends(require_api_key), Depends(limit_rate)],
)
async def ingest(batch: IngestBatch, request: Request) -> IngestResponse:
    check_timestamps(batch.samples, time.time())
    rows = [(s.ts, s.metric, s.device, s.value) for s in batch.samples]
    from starlette.concurrency import run_in_threadpool

    accepted = await run_in_threadpool(db.insert_samples, batch.host, rows)
    return IngestResponse(accepted=accepted)


__all__ = ["check_timestamps", "router"]
