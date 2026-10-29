import os
from pathlib import Path

import pytest

API_KEY = "test-key"


@pytest.fixture(autouse=True)
def _env(monkeypatch):
    monkeypatch.setenv("API_KEY", API_KEY)


@pytest.fixture
def pg():
    """Connection to a throwaway PostgreSQL database named by TEST_DATABASE_URL."""
    url = os.environ.get("TEST_DATABASE_URL")
    if not url:
        pytest.skip("TEST_DATABASE_URL not set")
    import psycopg
    from psycopg.rows import dict_row

    from app import db

    with psycopg.connect(url, autocommit=True) as c:
        if os.environ.get("TEST_DATABASE_KIND") == "timescale":
            # db/init.sql was applied by CI; just empty the tables between tests.
            c.execute("TRUNCATE metrics, alert_events, alert_rules RESTART IDENTITY CASCADE")
        else:
            c.execute((Path(__file__).parent / "schema_plain.sql").read_text())
    db.init_pool(url)
    conn = psycopg.connect(url, autocommit=True, row_factory=dict_row)
    yield conn
    conn.close()
    db.close_pool()


def refresh_rollups(conn) -> None:
    """Continuous aggregates are created WITH NO DATA; refresh them on real TimescaleDB."""
    if os.environ.get("TEST_DATABASE_KIND") == "timescale":
        for view in ("metrics_1m", "metrics_1h"):
            conn.execute(f"CALL refresh_continuous_aggregate('{view}', NULL, NULL)")
