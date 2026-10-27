from alerts.rules import (
    BREACH,
    FIRING,
    HEALTHY,
    NO_DATA,
    OK,
    Rule,
    evaluate_window,
    hosts_down,
    min_samples,
    should_repeat,
    transition,
)

RULE = Rule(id=1, name="CPU", metric="cpu.util_total", op=">", threshold=90, for_seconds=60)
NOW = 1000.0


def series(values, step=5.0, end=NOW):
    n = len(values)
    return [(end - (n - 1 - i) * step, v) for i, v in enumerate(values)]


def test_below_threshold_is_healthy():
    assert evaluate_window(RULE, series([50] * 13), NOW) == HEALTHY


def test_above_for_too_short_a_time_is_not_a_breach():
    # last 3 samples hot, but the rest of the 60 s window is below threshold
    assert evaluate_window(RULE, series([50] * 10 + [95] * 3), NOW) == HEALTHY


def test_above_for_whole_window_breaches():
    assert evaluate_window(RULE, series([95] * 13), NOW) == BREACH


def test_boundary_values_follow_operator():
    ge = Rule(id=2, name="x", metric="m", op=">=", threshold=98, for_seconds=10)
    assert evaluate_window(ge, series([98, 98, 98]), NOW) == BREACH
    gt = Rule(id=3, name="x", metric="m", op=">", threshold=98, for_seconds=10)
    assert evaluate_window(gt, series([98, 98, 98]), NOW) == HEALTHY


def test_sparse_window_is_no_data():
    assert evaluate_window(RULE, series([95] * 3), NOW) == NO_DATA
    assert evaluate_window(RULE, [], NOW) == NO_DATA


def test_min_samples_is_60_percent_of_expected():
    assert min_samples(60) == 8  # 12 expected -> ceil(7.2)
    assert min_samples(0) == 1


def test_fires_once_then_resolves_once():
    state, events = OK, []
    for verdict in [BREACH, BREACH, BREACH, HEALTHY, HEALTHY]:
        state, ev = transition(state, verdict)
        if ev:
            events.append(ev)
    assert events == ["firing", "resolved"]
    assert state == OK


def test_no_data_never_changes_state():
    assert transition(FIRING, NO_DATA) == (FIRING, None)
    assert transition(OK, NO_DATA) == (OK, None)


def test_repeat_interval():
    assert not should_repeat(0, 1799)
    assert should_repeat(0, 1800)


def test_agent_down_after_60_seconds_of_silence():
    assert hosts_down({"a": 900.0, "b": 995.0}, NOW) == {"a"}
    assert hosts_down({"a": 940.0}, NOW) == set()
