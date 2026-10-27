"""Pure alert-evaluation logic. Nothing here touches a database or the network."""

import math
import operator
from dataclasses import dataclass

OPS = {">": operator.gt, ">=": operator.ge, "<": operator.lt, "<=": operator.le}

SAMPLE_INTERVAL_S = 5.0  # expected agent sampling period
MIN_COVERAGE = 0.6  # fraction of expected samples required in the window
NO_DATA_TIMEOUT_S = 60.0
REPEAT_AFTER_S = 30 * 60

OK, FIRING = "ok", "firing"
BREACH, HEALTHY, NO_DATA = "breach", "healthy", "no_data"


@dataclass(frozen=True)
class Rule:
    id: int
    name: str
    metric: str
    op: str
    threshold: float
    for_seconds: int = 120
    host: str | None = None
    device: str | None = None
    severity: str = "warning"
    notify_url: str | None = None


def min_samples(for_seconds: int, interval: float = SAMPLE_INTERVAL_S) -> int:
    return max(1, math.ceil(MIN_COVERAGE * for_seconds / interval))


def evaluate_window(
    rule: Rule, samples: list[tuple[float, float]], now: float, interval: float = SAMPLE_INTERVAL_S
) -> str:
    """Classify the last `for_seconds` of (ts, value) samples as breach/healthy/no_data.

    A breach needs every sample in the window to violate the threshold and enough
    samples to cover the window. Too few samples is `no_data`, never a state change.
    """
    cutoff = now - rule.for_seconds
    window = [v for ts, v in samples if cutoff <= ts <= now]
    if len(window) < min_samples(rule.for_seconds, interval):
        return NO_DATA
    test = OPS[rule.op]
    return BREACH if all(test(v, rule.threshold) for v in window) else HEALTHY


def transition(state: str, verdict: str) -> tuple[str, str | None]:
    """Return (new_state, event) where event is 'firing', 'resolved' or None."""
    if verdict == NO_DATA:
        return state, None
    if verdict == BREACH and state == OK:
        return FIRING, "firing"
    if verdict == HEALTHY and state == FIRING:
        return OK, "resolved"
    return state, None


def should_repeat(last_notified: float, now: float, repeat_after: float = REPEAT_AFTER_S) -> bool:
    return now - last_notified >= repeat_after


def hosts_down(
    last_seen: dict[str, float], now: float, timeout: float = NO_DATA_TIMEOUT_S
) -> set[str]:
    return {h for h, ts in last_seen.items() if now - ts > timeout}
