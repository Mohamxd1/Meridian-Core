import math
from typing import Literal
from urllib.parse import urlparse

from pydantic import BaseModel, Field, field_validator

METRIC_PATTERN = r"^[a-z0-9_.]+$"
MAX_SAMPLES = 5000


class Sample(BaseModel):
    ts: float
    metric: str = Field(pattern=METRIC_PATTERN, max_length=128)
    device: str = Field(default="", max_length=128)
    value: float

    @field_validator("ts", "value")
    @classmethod
    def _finite(cls, v: float) -> float:
        if not math.isfinite(v):
            raise ValueError("must be finite")
        return v


class IngestBatch(BaseModel):
    host: str = Field(min_length=1, max_length=253)
    agent_version: str = Field(default="", max_length=64)
    samples: list[Sample] = Field(max_length=MAX_SAMPLES)

    @field_validator("host")
    @classmethod
    def _host_not_blank(cls, v: str) -> str:
        if not v.strip():
            raise ValueError("host must not be blank")
        return v


class IngestResponse(BaseModel):
    accepted: int


class RuleIn(BaseModel):
    name: str = Field(min_length=1, max_length=200)
    host: str | None = None
    metric: str = Field(pattern=METRIC_PATTERN, max_length=128)
    device: str | None = None
    op: Literal[">", ">=", "<", "<="]
    threshold: float
    for_seconds: int = Field(default=120, ge=0, le=86400)
    severity: Literal["info", "warning", "critical"] = "warning"
    notify_url: str | None = None
    enabled: bool = True

    @field_validator("threshold")
    @classmethod
    def _finite(cls, v: float) -> float:
        if not math.isfinite(v):
            raise ValueError("must be finite")
        return v

    @field_validator("notify_url")
    @classmethod
    def _http_url(cls, v: str | None) -> str | None:
        if v in (None, ""):
            return None
        if urlparse(v).scheme not in ("http", "https") or not urlparse(v).netloc:
            raise ValueError("notify_url must be an http(s) URL")
        return v


class Rule(RuleIn):
    id: int
