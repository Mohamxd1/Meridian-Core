import os
import secrets
import time
from collections import deque

from fastapi import Header, HTTPException, Request
from starlette.responses import JSONResponse

MAX_BODY_BYTES = 1_000_000


def require_api_key(x_api_key: str | None = Header(default=None)) -> None:
    expected = os.environ.get("API_KEY", "")
    if not expected or x_api_key is None:
        raise HTTPException(401, "invalid API key")
    if not secrets.compare_digest(x_api_key.encode(), expected.encode()):
        raise HTTPException(401, "invalid API key")


class RateLimiter:
    """Sliding-window limiter for /ingest, keyed by client address."""

    def __init__(self, limit: int, window: float = 60.0):
        self.limit, self.window = limit, window
        self._hits: dict[str, deque[float]] = {}

    def check(self, key: str, now: float | None = None) -> bool:
        now = time.monotonic() if now is None else now
        q = self._hits.setdefault(key, deque())
        while q and q[0] <= now - self.window:
            q.popleft()
        if len(q) >= self.limit:
            return False
        q.append(now)
        return True


ingest_limiter = RateLimiter(int(os.environ.get("INGEST_RATE_PER_MINUTE", "600")))


async def limit_rate(request: Request) -> None:
    client = request.client.host if request.client else "unknown"
    if not ingest_limiter.check(client):
        raise HTTPException(429, "rate limit exceeded")


class BodyLimitMiddleware:
    """Reject bodies over MAX_BODY_BYTES on /ingest, by Content-Length and by streamed size."""

    def __init__(self, app, limit: int = MAX_BODY_BYTES):
        self.app, self.limit = app, limit

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http" or scope["path"] != "/ingest":
            return await self.app(scope, receive, send)
        headers = dict(scope["headers"])
        declared = headers.get(b"content-length", b"")
        if declared.isdigit() and int(declared) > self.limit:
            return await self._reject(scope, receive, send)
        seen = 0

        async def counting_receive():
            nonlocal seen
            msg = await receive()
            if msg["type"] == "http.request":
                seen += len(msg.get("body", b""))
                if seen > self.limit:
                    raise _TooLarge
            return msg

        try:
            await self.app(scope, counting_receive, send)
        except _TooLarge:
            await self._reject(scope, receive, send)

    async def _reject(self, scope, receive, send):
        resp = JSONResponse({"detail": "request body too large"}, status_code=413)
        await resp(scope, receive, send)


class _TooLarge(Exception):
    pass
