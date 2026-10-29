import os
from contextlib import asynccontextmanager

from fastapi import FastAPI

from . import db, routes_ingest, routes_query, routes_rules, sse
from .security import BodyLimitMiddleware


@asynccontextmanager
async def lifespan(app: FastAPI):
    if os.environ.get("DATABASE_URL"):
        db.init_pool()
    try:
        yield
    finally:
        db.close_pool()


app = FastAPI(title="Meridian Core API", version="0.1.0", lifespan=lifespan)
app.add_middleware(BodyLimitMiddleware)
app.include_router(routes_ingest.router)
app.include_router(routes_query.router)
app.include_router(routes_rules.router)
app.include_router(sse.router)


@app.get("/health")
def health() -> dict:
    return {"status": "ok"}
