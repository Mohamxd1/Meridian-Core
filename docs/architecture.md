# Architecture

See README section 2 for the diagram. This note records how the code maps to it.

| Component | Path | Notes |
|---|---|---|
| Agent | `agent/` | C11. `ring.c` (bounded buffer, drops oldest), `json_out.c`, `cpu_math.c`, `config.c`, `http_post.c` (libcurl, 5 s timeout), one `collect_*.c` per metric family with `#ifdef _WIN32` bodies. `main.c` collects every `INTERVAL_SECONDS`, flushes every `BATCH_SECONDS`, backs off 1 s → 30 s after a failed POST, and removes samples from the ring only after a 2xx. |
| API | `api/app/` | FastAPI. `/ingest` (key, rate limit, 1 MB body cap, timestamp window), `/api/*` queries, rules CRUD (key on writes), SSE stream. SQL is parameterized; the only interpolated identifiers are fixed table names chosen by `db.pick_source`. |
| Alerts | `api/alerts/` | `rules.py` is pure (window evaluation, state machine, agent-down). `worker.py` runs it every 15 s against the database and restores firing state after a restart. `notifiers.py` sends Discord/Slack webhooks and optional SMTP email. |
| Storage | `db/init.sql` | Hypertable, 1 m / 1 h continuous aggregates, retention (7 d / 90 d / 2 y), alert tables, starter rules. |
| Dashboard | `dashboard/` | React + Vite + uPlot, served by nginx which proxies `/api` and `/ingest`. |

## Decisions

- **No-data is not a state change.** If a window holds fewer than 60% of the expected samples, the rule neither fires nor resolves; the separate `agent.down` alert covers a silent host. An `agent.down` rule row (`metric = 'agent.down'`) is created by the worker on first run so its events satisfy the `alert_events.rule_id` foreign key.
- **Query step.** The API never returns finer than the source resolution or more than 2,000 buckets. The chosen table and step are returned in `X-Source-Table` and `X-Step-Seconds`.
- **Webhook URLs are secrets.** They are never logged; delivery failures log only the exception type.
- **Redis** is provisioned by Compose but unused: the optional ingestion queue (README 9.2) was not implemented because direct `COPY` writes are sufficient at this scale.
- **GPU identity** uses the NVML index (`gpu0`, …); NVML does not guarantee stable ordering across reboots.
- **PDH GPU fallback** reports one aggregate device, `gpu0`, summing 3D engines and dedicated VRAM.

## Testing layout

| Layer | Command | Needs |
|---|---|---|
| Agent unit + mock-server integration | `cmake -S agent -B agent/build -DMERIDIAN_SANITIZE=ON && cmake --build agent/build && ctest --test-dir agent/build` | gcc/clang, libcurl dev, Python 3 |
| API + alerts (pure) | `cd api && pytest` | none |
| API + alerts (database) | `TEST_DATABASE_URL=postgresql://... pytest` | PostgreSQL. Without `TEST_DATABASE_KIND=timescale` the fixture builds a plain-PostgreSQL stand-in schema (`api/tests/schema_plain.sql`, with `time_bucket` shimmed by `date_bin`). With it, tests run against the schema from `db/init.sql`, as CI does. |
| Dashboard | `cd dashboard && npm test && npm run build` | Node 20 |
