# Meridian Core
# Meridian Core — Local Infrastructure Monitoring (a mini Datadog)

Meridian Core is a self-hosted monitoring system that runs on your own machine. A native **agent** samples CPU, memory, disk, network, and GPU metrics; an **API** ingests and stores them as time-series data; an **alert engine** watches thresholds; and a **live dashboard** charts everything with history.

It reproduces, at small scale, what Datadog does for CPU and GPU monitoring: collection, storage with retention, alerting, and visualization. Task Manager shows one machine for about a minute; Meridian Core keeps history, alerts when you are not looking, and can later watch many hosts.

---

## Table of contents

1. [Goals and non-goals](#1-goals-and-non-goals)
2. [Architecture](#2-architecture)
3. [Tech stack](#3-tech-stack)
4. [Prerequisites](#4-prerequisites)
5. [Repository layout](#5-repository-layout)
6. [Metric catalog](#6-metric-catalog)
7. [Data contract](#7-data-contract)
8. [Component 1: Agent (C)](#8-component-1-agent-c)
9. [Component 2: API (FastAPI)](#9-component-2-api-fastapi)
10. [Component 3: Storage (TimescaleDB)](#10-component-3-storage-timescaledb)
11. [Component 4: Alert engine](#11-component-4-alert-engine)
12. [Component 5: Dashboard](#12-component-5-dashboard)
13. [Docker Compose deployment](#13-docker-compose-deployment)
14. [Configuration](#14-configuration)
15. [Build plan with acceptance criteria](#15-build-plan-with-acceptance-criteria)
16. [Testing](#16-testing)
17. [Security](#17-security)
18. [Troubleshooting](#18-troubleshooting)
19. [Stretch goals](#19-stretch-goals)
20. [References](#20-references)
21. [Appendix A: supporting files](#21-appendix-a-supporting-files)
22. [Appendix B: first-run checklist](#22-appendix-b-first-run-checklist)

---

## Implementation status

Every component in this document is implemented in this repository. Quick start:

```
cp .env.example .env && docker compose up -d --build     # db, redis, api, alerts, dashboard
cmake -S agent -B agent/build && cmake --build agent/build
API_URL=http://localhost:8000/ingest API_KEY=<your key> HOST_NAME=$(hostname) agent/build/meridian-agent
```

`agent/build/meridian-agent --print` prints a local readout every 2 s (build phase 1) and sends nothing.
Details of the code layout and decisions are in [docs/architecture.md](docs/architecture.md).

**Verified in the development container (Linux, no GPU, no Docker daemon):**

- Agent: C unit tests and a mock-HTTP-server integration test (payload shape, API-key header, outage with
  buffered redelivery, clean SIGTERM) pass under AddressSanitizer + UBSan with no warnings.
- API and alert engine: 35 pytest tests pass against a real PostgreSQL 16 server, using a plain-PostgreSQL
  stand-in schema (no TimescaleDB extension available there).
- A real agent process posted to a running API backed by PostgreSQL; `/api/hosts`, `/api/metrics`,
  `/api/query`, `/api/stream` (SSE) and the 401 response for a bad key were exercised with curl.
- Dashboard: type-check, 8 Vitest tests, and a production build pass.

**Not verified here, and therefore not claimed:**

- `db/init.sql` against a real TimescaleDB server, and `docker compose up` (no Docker daemon). CI applies
  `init.sql` to a TimescaleDB service container and runs the API tests against it; that run has not happened yet.
- The Windows code paths (PDH, `GlobalMemoryStatusEx`, `GetIfTable2`, PDH GPU counters) were written to the
  APIs in sections 8.2–8.6 but never compiled or run. The Windows CI job is the first compile.
- NVML collection: the machine has no NVIDIA GPU, so only the "library absent → `gpu=none`" path ran.
- The phase acceptance checks that need human-driven load or external services: values within a few percent of
  Task Manager/`nvidia-smi`, a 7-day chart loading in under 1 s, the stress-test firing/resolved Discord
  messages, and the browser rendering (the dashboard was built but not viewed in a browser).
- No benchmark numbers are recorded for this project.

---

## 1. Goals and non-goals

**Goals**

- Collect CPU, memory, disk, network, and GPU metrics every few seconds.
- Store them as time-series data with automatic downsampling and retention.
- Show live and historical charts in a browser.
- Fire alerts (Discord/Slack webhook or email) when thresholds are crossed for a sustained period.
- Run on your own device with one command for everything except the agent.

**Non-goals (for v1)**

- Distributed tracing, log management, or APM.
- Multi-tenant accounts and billing.
- Autoscaling or cloud deployment.

---

## 2. Architecture

```
┌──────────────────────────── Host machine ────────────────────────────┐
│                                                                      │
│  ┌─────────────┐   HTTP POST /ingest    ┌─────────────────────────┐  │
│  │  Agent (C)  │ ─────────────────────► │   API (FastAPI)         │  │
│  │  runs native│   JSON batch, API key  │   validate + write      │  │
│  │  on the host│                        └───────┬─────────┬───────┘  │
│  └─────────────┘                                │         │          │
│   reads: PDH / /proc, NVML                      ▼         ▼          │
│                                       ┌──────────────┐ ┌─────────┐   │
│                                       │ TimescaleDB  │ │  Redis  │   │
│                                       │ (hypertable) │ │ (queue, │   │
│                                       └──────┬───────┘ │  opt.)  │   │
│                                              │         └─────────┘   │
│                         ┌────────────────────┼────────────┐          │
│                         ▼                    ▼            │          │
│                ┌────────────────┐   ┌──────────────────┐  │          │
│                │ Alert engine   │   │ Query API + SSE  │  │          │
│                │ (Python worker)│   │ (FastAPI)        │  │          │
│                └───────┬────────┘   └────────┬─────────┘  │          │
│                        ▼                     ▼            │          │
│                 Discord/Slack/SMTP    Dashboard (browser) ◄┘          │
└──────────────────────────────────────────────────────────────────────┘
```

**Key design decision:** the agent runs **natively on the host**, not in a container. A container cannot see the real hardware counters or the GPU driver without extra passthrough, so everything else runs in Docker and the agent talks to it over `localhost`.

---

## 3. Tech stack

| Layer | Choice | Why |
|---|---|---|
| Agent | **C** (C11) + libcurl | Small, fast, direct access to OS and driver APIs |
| CPU/memory (Windows) | PDH (Performance Data Helper) + `GlobalMemoryStatusEx` | Same counters Task Manager and Perfmon use |
| CPU/memory (Linux) | `/proc/stat`, `/proc/meminfo`, `/proc/net/dev`, `statvfs` | No dependencies |
| GPU (NVIDIA) | **NVML**, loaded dynamically | Utilization, memory, temperature, power |
| GPU (any vendor, Windows) | PDH `GPU Engine` / `GPU Adapter Memory` counters | Vendor-neutral fallback; what Task Manager uses |
| GPU (AMD, Linux) | `rocm-smi` or sysfs (`/sys/class/drm/card*/device/`) | Native Linux path |
| API | **Python 3.11+**, FastAPI, Uvicorn, Pydantic, psycopg 3 | You already use FastAPI |
| Storage | **TimescaleDB** (PostgreSQL extension; now published by Tiger Data) | Hypertables, continuous aggregates, retention policies |
| Queue (optional) | Redis Streams | Buffer ingestion bursts |
| Alerting | Python worker + `httpx` (webhooks) + `smtplib` (email) | Simple and testable |
| Dashboard | React + Vite + **uPlot** (or plain HTML/CSS/SVG) | uPlot is built for fast time-series charts |
| Live updates | Server-Sent Events (SSE) | One-way push, simpler than WebSockets |
| Packaging | Docker + Docker Compose | One-command deployment |
| Build tooling | CMake (agent), `uv` or `pip` (API), npm (dashboard) | Standard choices |
| CI | GitHub Actions | Build and test on every push |
| Tests | `pytest`, `pytest-asyncio`, Unity or CTest (C) | Cover agent math and the rules engine |

Optional comparison tools: **Prometheus + Grafana** (the open-source equivalent). Run them once to see what a mature system produces and compare against yours.

---

## 4. Prerequisites

Install these before starting.

**All platforms**

- Git
- Docker Desktop (Windows/macOS) or Docker Engine + Compose plugin (Linux)
- Python 3.11 or newer
- Node.js 20 or newer
- CMake 3.20 or newer

**Windows (agent)**

- Visual Studio Build Tools (MSVC) with the "Desktop development with C++" workload
- **vcpkg** for libcurl: `vcpkg install curl:x64-windows`
- For NVIDIA GPUs: a current NVIDIA driver (it provides `nvml.dll`)

**Linux (agent)**

- `gcc` or `clang`, `make`, `libcurl4-openssl-dev`
- For NVIDIA GPUs: the NVIDIA driver (provides `libnvidia-ml.so.1`)

**Know your GPU vendor first.** It decides the GPU collection path:

| GPU | Path |
|---|---|
| NVIDIA | NVML (best detail: power, temperature, memory) |
| AMD or Intel on Windows | PDH GPU counters (utilization and memory only; no reliable power/temperature) |
| AMD on Linux | `rocm-smi` or sysfs |

---

## 5. Repository layout

```
meridian-core/
├── README.md
├── docker-compose.yml
├── .env.example
├── agent/
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── main.c              # loop, timing, batching
│   │   ├── collect_cpu.c       # PDH (Windows) or /proc/stat (Linux)
│   │   ├── collect_mem.c
│   │   ├── collect_disk.c
│   │   ├── collect_net.c
│   │   ├── collect_gpu_nvml.c  # dynamic NVML loader
│   │   ├── collect_gpu_pdh.c   # Windows vendor-neutral fallback
│   │   ├── json_out.c          # builds the JSON batch
│   │   └── http_post.c         # libcurl wrapper with retry
│   ├── include/
│   └── tests/
├── api/
│   ├── pyproject.toml
│   ├── app/
│   │   ├── main.py             # FastAPI app
│   │   ├── routes_ingest.py
│   │   ├── routes_query.py
│   │   ├── routes_rules.py
│   │   ├── sse.py
│   │   ├── db.py
│   │   └── schemas.py
│   ├── alerts/
│   │   ├── worker.py           # evaluation loop
│   │   ├── rules.py
│   │   └── notifiers.py        # webhook + email
│   └── tests/
├── db/
│   └── init.sql                # extension, tables, policies
├── dashboard/
│   ├── package.json
│   └── src/
└── docs/
    └── architecture.md
```

---

## 6. Metric catalog

Use consistent names from day one. Units are fixed so charts never guess.

| Metric | Unit | Device label | Windows source | Linux source |
|---|---|---|---|---|
| `cpu.util_total` | % (0–100) | `total` | PDH `\Processor(_Total)\% Processor Time` | `/proc/stat` deltas |
| `cpu.util_core` | % | `0`, `1`, … | PDH `\Processor(n)\% Processor Time` | `/proc/stat` per-core deltas |
| `mem.used_pct` | % | — | `GlobalMemoryStatusEx` | `/proc/meminfo` |
| `mem.used_bytes` | bytes | — | `GlobalMemoryStatusEx` | `/proc/meminfo` |
| `disk.used_pct` | % | drive/mount | `GetDiskFreeSpaceEx` | `statvfs` |
| `net.rx_bytes_per_s` / `net.tx_bytes_per_s` | bytes/s | interface | PDH `\Network Interface(*)\Bytes Received/sec` | `/proc/net/dev` deltas |
| `gpu.util` | % | `gpu0`, … | NVML `nvmlDeviceGetUtilizationRates().gpu` | same |
| `gpu.mem_util` | % | `gpu0` | NVML utilization `.memory` | same |
| `gpu.mem_used_bytes` | bytes | `gpu0` | NVML `nvmlDeviceGetMemoryInfo().used` | same |
| `gpu.mem_total_bytes` | bytes | `gpu0` | NVML `nvmlDeviceGetMemoryInfo().total` | same |
| `gpu.temp_c` | °C | `gpu0` | NVML `nvmlDeviceGetTemperature(dev, NVML_TEMPERATURE_GPU, &t)` | same |
| `gpu.power_w` | watts | `gpu0` | NVML `nvmlDeviceGetPowerUsage` (**returns milliwatts; divide by 1000**) | same |

**Rules of thumb**

- CPU percentages from deltas (`/proc/stat`, PDH) need **two samples** before the first valid value. The first reading is discarded.
- Store the value as a `double`, in the unit shown above, so the dashboard and alerts never convert.

---

## 7. Data contract

The agent sends batches. One request carries many samples to keep overhead low.

**`POST /ingest`**

Headers: `Content-Type: application/json`, `X-API-Key: <key>`

```json
{
  "host": "johnny-desktop",
  "agent_version": "0.1.0",
  "samples": [
    { "ts": 1761573600.000, "metric": "cpu.util_total", "device": "total", "value": 23.5 },
    { "ts": 1761573600.000, "metric": "gpu.util",       "device": "gpu0",  "value": 71.0 },
    { "ts": 1761573600.000, "metric": "gpu.temp_c",     "device": "gpu0",  "value": 64.0 },
    { "ts": 1761573600.000, "metric": "gpu.power_w",    "device": "gpu0",  "value": 182.4 }
  ]
}
```

- `ts` is Unix epoch seconds (UTC), float.
- Responses: `202 Accepted` with `{"accepted": N}`; `401` for a bad key; `422` for a malformed body.
- The agent must **retry with backoff** on network errors and keep unsent batches in a small in-memory ring buffer so brief outages do not lose data.

---

## 8. Component 1: Agent (C)

### 8.1 Main loop

1. Read config (host name, API URL, API key, interval).
2. Initialize collectors (PDH query, NVML if available).
3. Every `INTERVAL_SECONDS` (default 5): collect all metrics, append to a batch.
4. Every `BATCH_SECONDS` (default 10): serialize and POST. On failure, keep the batch and retry next cycle.

### 8.2 CPU on Windows (PDH)

PDH needs the **English counter name** function so the program works on any Windows language, and it needs **two** `PdhCollectQueryData` calls before a percentage is valid.

```c
#include <windows.h>
#include <pdh.h>
#pragma comment(lib, "pdh.lib")

static PDH_HQUERY   q;
static PDH_HCOUNTER cpu_total;

int cpu_init(void) {
    if (PdhOpenQuery(NULL, 0, &q) != ERROR_SUCCESS) return -1;
    if (PdhAddEnglishCounterA(q, "\\Processor(_Total)\\% Processor Time",
                              0, &cpu_total) != ERROR_SUCCESS) return -1;
    PdhCollectQueryData(q);              // priming sample; value not valid yet
    return 0;
}

int cpu_read(double *out_pct) {
    PDH_FMT_COUNTERVALUE v;
    if (PdhCollectQueryData(q) != ERROR_SUCCESS) return -1;
    if (PdhGetFormattedCounterValue(cpu_total, PDH_FMT_DOUBLE, NULL, &v) != ERROR_SUCCESS)
        return -1;
    *out_pct = v.doubleValue;
    return 0;
}
```

Link `pdh.lib`. Per-core counters use `\Processor(0)\% Processor Time`, `\Processor(1)\...`; get the core count from `GetSystemInfo`.

### 8.3 CPU on Linux (`/proc/stat`)

Read the first `cpu` line, then compute utilization from the **difference** between two reads:

```
busy_delta  = (user+nice+system+irq+softirq+steal)_now - (...)_prev
total_delta = busy_delta + (idle+iowait)_now - (idle+iowait)_prev
util_pct    = 100.0 * busy_delta / total_delta
```

### 8.4 GPU via NVML (dynamic loading)

Load the library at runtime so the same agent binary still runs on machines with no NVIDIA driver.

- Windows: `LoadLibraryA("nvml.dll")`. It ships with the NVIDIA driver; older installs also have it under `C:\Program Files\NVIDIA Corporation\NVSMI\`. Try the default search path first, then that folder.
- Linux: `dlopen("libnvidia-ml.so.1", RTLD_NOW)`.

Functions to resolve (via `GetProcAddress` / `dlsym`): `nvmlInit_v2`, `nvmlShutdown`, `nvmlDeviceGetCount_v2`, `nvmlDeviceGetHandleByIndex_v2`, `nvmlDeviceGetName`, `nvmlDeviceGetUtilizationRates`, `nvmlDeviceGetMemoryInfo`, `nvmlDeviceGetTemperature`, `nvmlDeviceGetPowerUsage`.

```c
typedef int (*nvmlInit_t)(void);
typedef int (*nvmlGetCount_t)(unsigned int *);
typedef int (*nvmlGetHandle_t)(unsigned int, void **);
typedef struct { unsigned int gpu; unsigned int memory; } nvmlUtilization_t;
typedef int (*nvmlGetUtil_t)(void *, nvmlUtilization_t *);
typedef int (*nvmlGetTemp_t)(void *, int /*sensor*/, unsigned int *);
typedef int (*nvmlGetPower_t)(void *, unsigned int *);   // milliwatts

// per GPU, each cycle:
//   nvmlUtilization_t u; getUtil(h, &u);   -> gpu.util = u.gpu, gpu.mem_util = u.memory
//   unsigned int t;      getTemp(h, 0, &t);-> gpu.temp_c (sensor 0 = NVML_TEMPERATURE_GPU)
//   unsigned int mw;     getPower(h, &mw); -> gpu.power_w = mw / 1000.0
```

For production use, prefer including NVIDIA's `nvml.h` (from the CUDA Toolkit) for exact struct and enum definitions instead of hand-written typedefs, but still resolve the functions dynamically. NVML return code `0` is success. Handle `NVML_ERROR_NOT_SUPPORTED` per metric (some GPUs do not expose power or temperature) by skipping that metric instead of failing the whole cycle.

Notes from NVML's documentation: device enumeration order is **not** guaranteed to stay the same across reboots, so label GPUs by UUID or name plus index if you need stable identity; and fan speed is generally unavailable on some data-center part classes.

### 8.5 GPU on Windows without NVML (any vendor)

Windows exposes per-engine counters. Use wildcard expansion (`PdhExpandWildCardPathA`) to enumerate instances, then sum:

- `\GPU Engine(*)\Utilization Percentage` — sum instances whose name contains `engtype_3D` (this approximates the "3D" graph in Task Manager)
- `\GPU Adapter Memory(*)\Dedicated Usage` — bytes of VRAM in use

This path gives utilization and memory but **not** temperature or power.

### 8.6 Memory, disk, network

- Windows memory: `MEMORYSTATUSEX` → `dwMemoryLoad`, `ullTotalPhys - ullAvailPhys`.
- Disk: `GetDiskFreeSpaceExA` per fixed drive.
- Network: PDH `\Network Interface(*)\Bytes Received/sec` and `Bytes Sent/sec`, or compute deltas from `GetIfTable2`.

### 8.7 JSON and HTTP

- Build JSON with a tiny writer (`snprintf` into a buffer) or **cJSON**.
- POST with **libcurl**: set `CURLOPT_TIMEOUT` (5 s), `CURLOPT_HTTPHEADER` (`Content-Type`, `X-API-Key`), and check the HTTP status.
- Retry with exponential backoff (1 s, 2 s, 4 s, up to 30 s). Cap the in-memory buffer (for example 1,000 samples) and drop the oldest when full.

### 8.8 Build

```
cd agent
cmake -B build -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake   # Windows
cmake -B build                                                                         # Linux
cmake --build build --config Release
```

### 8.9 Run as a service

- Windows: wrap with **NSSM** or create a Scheduled Task ("At log on", restart on failure).
- Linux: a `systemd` unit with `Restart=always`.

---

## 9. Component 2: API (FastAPI)

### 9.1 Endpoints

| Method | Path | Purpose |
|---|---|---|
| `POST` | `/ingest` | Receive an agent batch (API key required) |
| `GET` | `/health` | Liveness check (no auth) |
| `GET` | `/api/hosts` | List known hosts and last-seen time |
| `GET` | `/api/metrics` | List metric names and devices for a host |
| `GET` | `/api/query` | Time-series query: `host`, `metric`, `device`, `from`, `to`, `step` |
| `GET` | `/api/stream` | SSE stream of the latest values for a host |
| `GET` / `POST` / `PUT` / `DELETE` | `/api/rules` | Manage alert rules |
| `GET` | `/api/alerts` | Alert history and currently firing alerts |

### 9.2 Ingest behavior

- Validate with Pydantic (`host` non-empty, `metric` matches `^[a-z0-9_.]+$`, `value` finite).
- Write the whole batch with a single `COPY` or `executemany` call. Use a connection pool.
- Reject timestamps more than 5 minutes in the future or 7 days in the past.
- Optional: push to a Redis Stream and have a writer task drain it, so spikes never block the agent.

### 9.3 Query behavior

Choose the table by time range, which keeps charts fast:

| Range requested | Source | Resolution |
|---|---|---|
| ≤ 6 hours | raw `metrics` table | native (5 s) |
| 6 hours – 7 days | `metrics_1m` aggregate | 1 minute |
| > 7 days | `metrics_1h` aggregate | 1 hour |

Use `time_bucket(step, time)` and return `avg`, `min`, and `max` per bucket.

### 9.4 SSE

`/api/stream?host=...` holds the connection open and emits one event per second with the latest value of each metric, using `text/event-stream`. In FastAPI use `StreamingResponse` (or `sse-starlette`).

### 9.5 Run locally

```
cd api
python -m venv .venv && . .venv/bin/activate     # Windows: .venv\Scripts\activate
pip install -e .
uvicorn app.main:app --reload --port 8000
```

---

## 10. Component 3: Storage (TimescaleDB)

TimescaleDB is a PostgreSQL extension, so you keep normal SQL and the PostgreSQL skills you already have. (The project is now published under the **Tiger Data** name; the extension and SQL functions are unchanged, but documentation has moved to `tigerdata.com`.)

### 10.1 `db/init.sql`

```sql
CREATE EXTENSION IF NOT EXISTS timescaledb;

-- Raw samples
CREATE TABLE IF NOT EXISTS metrics (
    time    timestamptz      NOT NULL,
    host    text             NOT NULL,
    metric  text             NOT NULL,
    device  text             NOT NULL DEFAULT '',
    value   double precision NOT NULL
);

SELECT create_hypertable('metrics', 'time', if_not_exists => TRUE);

CREATE INDEX IF NOT EXISTS metrics_lookup
    ON metrics (host, metric, device, time DESC);

-- 1-minute rollup
CREATE MATERIALIZED VIEW IF NOT EXISTS metrics_1m
WITH (timescaledb.continuous) AS
SELECT time_bucket('1 minute', time) AS bucket,
       host, metric, device,
       avg(value) AS avg, min(value) AS min, max(value) AS max
FROM metrics
GROUP BY bucket, host, metric, device
WITH NO DATA;

-- 1-hour rollup
CREATE MATERIALIZED VIEW IF NOT EXISTS metrics_1h
WITH (timescaledb.continuous) AS
SELECT time_bucket('1 hour', time) AS bucket,
       host, metric, device,
       avg(value) AS avg, min(value) AS min, max(value) AS max
FROM metrics
GROUP BY bucket, host, metric, device
WITH NO DATA;

-- Keep the aggregates fresh
SELECT add_continuous_aggregate_policy('metrics_1m',
    start_offset      => INTERVAL '3 days',
    end_offset        => INTERVAL '1 minute',
    schedule_interval => INTERVAL '1 minute');

-- NOTE: start_offset must stay SHORTER than raw retention (7 days below),
-- otherwise a refresh can wipe aggregate history. See warning under this block.
SELECT add_continuous_aggregate_policy('metrics_1h',
    start_offset      => INTERVAL '3 days',
    end_offset        => INTERVAL '1 hour',
    schedule_interval => INTERVAL '1 hour');

-- Retention: raw 7 days, 1-minute rollup 90 days, 1-hour rollup 2 years
SELECT add_retention_policy('metrics',    drop_after => INTERVAL '7 days');
SELECT add_retention_policy('metrics_1m', drop_after => INTERVAL '90 days');
SELECT add_retention_policy('metrics_1h', drop_after => INTERVAL '2 years');

-- Alerting tables
CREATE TABLE IF NOT EXISTS alert_rules (
    id           serial PRIMARY KEY,
    name         text    NOT NULL,
    host         text,                         -- NULL = all hosts
    metric       text    NOT NULL,
    device       text,                         -- NULL = any device
    op           text    NOT NULL CHECK (op IN ('>','>=','<','<=')),
    threshold    double precision NOT NULL,
    for_seconds  integer NOT NULL DEFAULT 120, -- must hold this long
    severity     text    NOT NULL DEFAULT 'warning',
    notify_url   text,                         -- webhook target
    enabled      boolean NOT NULL DEFAULT true
);

CREATE TABLE IF NOT EXISTS alert_events (
    id         bigserial PRIMARY KEY,
    rule_id    integer REFERENCES alert_rules(id) ON DELETE CASCADE,
    host       text NOT NULL,
    device     text NOT NULL DEFAULT '',
    state      text NOT NULL CHECK (state IN ('firing','resolved')),
    value      double precision,
    at         timestamptz NOT NULL DEFAULT now()
);
```

> **Retention warning (important).** A retention policy on the raw table must reach **further back than the refresh window of the continuous aggregate**. A 2026 Tiger Data write-up shows that if retention (for example 3 days) is shorter than the refresh `start_offset` (for example 7 days), the aggregate silently loses history with no error. In this schema raw retention is **7 days** and the refresh `start_offset` for both aggregates is **3 days**, which leaves a safety margin if a refresh job is missed. Keep that relationship (every refresh `start_offset` comfortably shorter than raw retention) if you change any number, and put a retention policy on each aggregate as shown. The aggregates keep their own, longer history because refreshes never reach back to the buckets whose raw data has already been dropped.

> Also: `add_retention_policy` drops whole **chunks**, so data may be kept slightly longer than the interval but never shorter. Only one retention policy can exist per hypertable.

Apply the file automatically by mounting `db/init.sql` into `/docker-entrypoint-initdb.d/` (see Compose below). It runs only the first time the data volume is created.

---

## 11. Component 4: Alert engine

A Python worker that evaluates rules on a timer (every 15 seconds).

### 11.1 Evaluation logic

For each enabled rule:

1. Query the raw table for the last `for_seconds` of samples matching `host`, `metric`, `device`.
2. The rule **breaches** only if **every** sample in the window satisfies `value <op> threshold` and the window has enough samples (for example at least 60% of the expected count, so a dead agent does not look healthy or broken).
3. State machine per (rule, host, device):
   - `ok` → `firing` when the breach condition becomes true: insert an `alert_events` row, send a notification.
   - `firing` → `resolved` when it is no longer true: insert a `resolved` event, send a recovery notification.
   - Do **not** re-notify every cycle while still firing (optionally repeat every 30 minutes).
4. **No-data alert:** if a host has sent nothing for 60 seconds, fire `agent.down` for that host.

### 11.2 Starter rules

| Name | Metric | Condition | For |
|---|---|---|---|
| GPU hot | `gpu.temp_c` | `> 85` | 2 min |
| GPU pegged | `gpu.util` | `>= 98` | 10 min |
| CPU saturated | `cpu.util_total` | `> 90` | 5 min |
| Memory pressure | `mem.used_pct` | `> 92` | 5 min |
| Disk almost full | `disk.used_pct` | `> 90` | 1 min |

### 11.3 Notifiers

- **Webhook** (Discord/Slack): `POST` JSON `{ "content": "🔥 GPU hot on johnny-desktop: 87 °C (rule: GPU hot)" }` (Discord) or `{ "text": "..." }` (Slack) via `httpx`. Keep webhook URLs in `.env` or the rule row, never in Git.
- **Email**: `smtplib` with TLS and an app password.

---

## 12. Component 5: Dashboard

### 12.1 Views

- **Hosts overview:** cards with current CPU %, memory %, GPU %, GPU temperature, and an "agent up/down" indicator.
- **Host detail:** line charts for CPU (total + per core), memory, GPU utilization, GPU memory, GPU temperature, GPU power, network, disk. Time-range picker (15 min, 1 h, 6 h, 24 h, 7 d, 30 d).
- **Alerts:** firing alerts at the top, history table below, rule editor.

### 12.2 Implementation notes

- `npm create vite@latest dashboard -- --template react-ts`
- Charts: **uPlot** (fast with tens of thousands of points) or Recharts for a quicker start.
- Live panel: `new EventSource('/api/stream?host=...')`.
- Historical charts: `fetch('/api/query?...')` and re-fetch on time-range change.
- Dark theme by default; use CSS variables so colors live in one place.
- Production build (`npm run build`) is served by an **nginx** container, which also proxies `/api` and `/ingest` to the API container so the browser sees one origin (no CORS setup needed).

---

## 13. Docker Compose deployment

`docker-compose.yml` (everything except the agent):

```yaml
services:
  db:
    image: timescale/timescaledb:latest-pg16
    environment:
      POSTGRES_PASSWORD: ${DB_PASSWORD}
      POSTGRES_DB: meridian
    volumes:
      - dbdata:/var/lib/postgresql/data
      - ./db/init.sql:/docker-entrypoint-initdb.d/init.sql:ro
    healthcheck:
      test: ["CMD-SHELL", "pg_isready -U postgres -d meridian"]
      interval: 5s
      retries: 10
    ports: ["127.0.0.1:5432:5432"]

  redis:
    image: redis:7-alpine
    ports: ["127.0.0.1:6379:6379"]

  api:
    build: ./api
    environment:
      DATABASE_URL: postgresql://postgres:${DB_PASSWORD}@db:5432/meridian
      REDIS_URL: redis://redis:6379/0
      API_KEY: ${API_KEY}
    depends_on:
      db: { condition: service_healthy }
    ports: ["127.0.0.1:8000:8000"]

  alerts:
    build: ./api
    command: python -m alerts.worker
    environment:
      DATABASE_URL: postgresql://postgres:${DB_PASSWORD}@db:5432/meridian
      DISCORD_WEBHOOK_URL: ${DISCORD_WEBHOOK_URL:-}
    depends_on:
      db: { condition: service_healthy }

  dashboard:
    build: ./dashboard          # multi-stage: node build -> nginx serve
    depends_on: [api]
    ports: ["127.0.0.1:8080:80"]

volumes:
  dbdata:
```

Start and stop:

```
cp .env.example .env        # then edit secrets
docker compose up -d --build
docker compose logs -f api
docker compose down         # keeps data; add -v to wipe it
```

Then start the agent on the host, pointing at `http://localhost:8000/ingest`, and open `http://localhost:8080`.

Docker Desktop on Windows runs containers in a VM, so "host" metrics must come from the **native agent**, which is why the agent is not in Compose.

---

## 14. Configuration

`.env.example`

```
DB_PASSWORD=change-me
API_KEY=change-me-long-random-string
DISCORD_WEBHOOK_URL=
```

Agent configuration (`agent.conf` or environment variables):

```
API_URL=http://localhost:8000/ingest
API_KEY=change-me-long-random-string
HOST_NAME=johnny-desktop
INTERVAL_SECONDS=5
BATCH_SECONDS=10
```

Generate a key: `python -c "import secrets; print(secrets.token_urlsafe(32))"`.

---

## 15. Build plan with acceptance criteria

Build in this order. Do not start a phase until the previous acceptance check passes.

| Phase | Deliverable | Acceptance check |
|---|---|---|
| **1. Local readout** | C program prints CPU %, memory %, and GPU util/temp/power every 2 s | Values are within a few percent of Task Manager (or `nvidia-smi`) while you run a load |
| **2. Ship it** | Agent POSTs JSON to a FastAPI `/ingest`; API writes to SQLite or Postgres | `SELECT count(*)` grows; killing the API and restarting it does not crash the agent |
| **3. Dashboard v1** | Page with one live CPU chart and one GPU chart | Chart moves in real time as you open a game or run a stress test |
| **4. TimescaleDB** | Move storage to the hypertable; add rollups and retention | `SELECT * FROM timescaledb_information.jobs;` shows the policies; 1-minute view contains data |
| **5. History queries** | `/api/query` picks raw vs 1m vs 1h by range | A 7-day chart loads in under 1 second |
| **6. Alerts** | Rules table, worker, Discord webhook | Run a CPU stress test for over 5 minutes: you get one "firing" message, then one "resolved" message |
| **7. Compose** | All services under `docker compose up` | Fresh clone + `.env` + one command = working system |
| **8. Hardening** | API key, localhost-only ports, tests, CI | `pytest` and CMake tests pass in GitHub Actions |
| **9. Polish** | Host overview, rule editor, README screenshots | Another person can set it up using only this README |

**Load-testing tools** for checking your numbers: `stress-ng` (Linux), a CPU-burn script in Python (`while True: pass` on all cores), FurMark or a game (GPU), `nvidia-smi dmon` to cross-check NVIDIA readings.

---

## 16. Testing

**Agent (C)**
- Unit-test the pure functions: `/proc/stat` delta math, JSON serialization (escape quotes, no trailing commas), ring buffer overflow behavior.
- Run under AddressSanitizer or Valgrind on Linux; use `/fsanitize=address` in MSVC.
- Integration test: run the agent for 30 s against a mock HTTP server and check payload shape.

**API (pytest)**
- Ingest: valid batch, bad key (401), malformed body (422), future timestamps rejected.
- Query: correct source table chosen per range; empty results return `[]`.
- Use a throwaway TimescaleDB container (`testcontainers-python` or a Compose profile).

**Alert engine (pytest)**
- Feed synthetic samples: below threshold, above for too short a time (no alert), above for long enough (fires once), recovery (resolved once), missing data (`agent.down`).
- This logic is pure; test it without a database by passing in sample lists.

**Dashboard**
- Basic component tests with Vitest; manual check of every time range.

**CI (`.github/workflows/ci.yml`)**
- Jobs: build agent on `ubuntu-latest` and `windows-latest`, run API tests with a TimescaleDB service container, build dashboard.

---

## 17. Security

- **API key** on `/ingest` and all write endpoints; compare with `secrets.compare_digest`.
- Bind all published ports to `127.0.0.1` (as in the Compose file). Only expose beyond localhost behind HTTPS and real authentication.
- Never commit `.env`, webhook URLs, or SMTP passwords; add `.env` to `.gitignore`.
- Use parameterized SQL everywhere (psycopg placeholders), never f-strings with user input.
- Rate-limit `/ingest` and cap request body size (for example 1 MB).
- The agent only reads metrics and holds no privileged credentials; run it as a normal user where possible.
- Dashboard behind a login if you ever expose it past your own machine.

---

## 18. Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| First CPU reading is 0 or an error | PDH and `/proc/stat` need two samples. Discard the first read. |
| CPU counter fails on non-English Windows | Use `PdhAddEnglishCounter`, not `PdhAddCounter`. |
| `nvml.dll` not found | Driver not installed, or DLL not on the search path. Try `C:\Program Files\NVIDIA Corporation\NVSMI\nvml.dll`, or fall back to PDH counters. |
| `gpu.power_w` is 1000× too large | NVML returns **milliwatts**; divide by 1000. |
| NVML returns "not supported" for power or temperature | Some GPUs don't expose it. Skip that metric. |
| GPU index changes after reboot | NVML order is not guaranteed. Label by UUID/name. |
| Containers cannot see real CPU/GPU | Expected. Run the agent natively on the host. |
| `init.sql` changes not applied | `/docker-entrypoint-initdb.d` only runs on first volume creation. Run `docker compose down -v` (wipes data) or apply the SQL manually with `psql`. |
| 1-minute aggregate is empty | Continuous aggregates were created `WITH NO DATA`. Wait for the policy to run, or call `CALL refresh_continuous_aggregate('metrics_1m', NULL, NULL);`. |
| Old history disappears from aggregates | Retention shorter than the refresh window. See the warning in section 10. |
| Dashboard charts stutter | Return downsampled data (`step`), limit to ~2,000 points per series, use uPlot. |
| Agent stops sending after Wi-Fi blip | Ensure retry/backoff and the ring buffer are implemented (section 8.7). |

---

## 19. Stretch goals

These tie into your other projects.

- **C++ analytics engine:** a service that reads the rollups and computes anomaly scores (z-score against a rolling baseline, EWMA) and a ranking of "most unusual hosts/metrics", reusing the ranking/graph ideas from **Meridian Rank**. Expose it as a gRPC or HTTP endpoint the API calls.
- **Orbit integration:** have the agent report container CPU/memory from Docker (`/containers/{id}/stats` through the Docker socket) so **Orbit**'s self-healing logic can restart unhealthy containers based on metrics from Meridian Core.
- **Per-process view:** top 10 processes by CPU/GPU memory (NVML `nvmlDeviceGetComputeRunningProcesses`).
- **Cost view:** estimate GPU energy cost from `gpu.power_w` and a price per kWh (mirrors Datadog's GPU cost attribution).
- **Multiple hosts:** run the agent on a second PC or a Raspberry Pi and compare in the dashboard (this is what makes it "fleet" monitoring).
- **Prometheus exporter:** expose `/metrics` so Grafana can read your data too.
- **Dashboards as code and screenshots** for the README and your portfolio.

---

## 20. References

- NVIDIA NVML API reference: https://docs.nvidia.com/deploy/nvml-api/index.html
- Windows PDH (collecting performance data): https://learn.microsoft.com/en-us/windows/win32/perfctrs/collecting-performance-data
- TimescaleDB / Tiger Data docs (hypertables, continuous aggregates, retention): https://www.tigerdata.com/docs
- `add_retention_policy`: https://www.tigerdata.com/docs/reference/timescaledb/data-retention/add_retention_policy
- Retention vs continuous aggregates pitfall: https://www.tigerdata.com/blog/how-retention-policies-silently-break-continuous-aggregates
- Datadog GPU Monitoring (the product this mimics): https://docs.datadoghq.com/gpu_monitoring
- Datadog NVIDIA DCGM integration blog: https://www.datadoghq.com/blog/monitor-nvidia-gpus-with-datadog.md
- FastAPI: https://fastapi.tiangolo.com
- uPlot: https://github.com/leeoniya/uPlot
- libcurl: https://curl.se/libcurl/

---

## 21. Appendix A: supporting files

The sections above reference these files; here are working starting points.

### `agent/CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.20)
project(meridian_agent C)
set(CMAKE_C_STANDARD 11)

file(GLOB SRC src/*.c)
add_executable(meridian-agent ${SRC})
target_include_directories(meridian-agent PRIVATE include)

find_package(CURL REQUIRED)
target_link_libraries(meridian-agent PRIVATE CURL::libcurl)

if(WIN32)
  target_link_libraries(meridian-agent PRIVATE pdh iphlpapi)
else()
  target_link_libraries(meridian-agent PRIVATE dl m)
endif()

enable_testing()
```

Because the source glob includes both Windows and Linux collectors, wrap each platform-specific file body in `#ifdef _WIN32 ... #else ... #endif` (or exclude files per platform in CMake).

### `api/pyproject.toml`

```toml
[project]
name = "meridian-api"
version = "0.1.0"
requires-python = ">=3.11"
dependencies = [
  "fastapi>=0.110",
  "uvicorn[standard]>=0.27",
  "pydantic>=2.6",
  "psycopg[binary,pool]>=3.1",
  "httpx>=0.27",
  "redis>=5.0",
  "sse-starlette>=2.0",
]

[project.optional-dependencies]
dev = ["pytest>=8", "pytest-asyncio>=0.23", "testcontainers>=4"]

[tool.setuptools]
packages = ["app", "alerts"]
```

### `api/Dockerfile`

```dockerfile
FROM python:3.12-slim
WORKDIR /app
COPY pyproject.toml .
COPY app ./app
COPY alerts ./alerts
RUN pip install --no-cache-dir .
EXPOSE 8000
CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000"]
```

### `dashboard/Dockerfile`

```dockerfile
FROM node:20-alpine AS build
WORKDIR /src
COPY package*.json ./
RUN npm ci
COPY . .
RUN npm run build

FROM nginx:alpine
COPY --from=build /src/dist /usr/share/nginx/html
COPY nginx.conf /etc/nginx/conf.d/default.conf
```

### `dashboard/nginx.conf`

```nginx
server {
  listen 80;
  root /usr/share/nginx/html;

  location / {
    try_files $uri /index.html;
  }

  # API + Server-Sent Events (buffering must be off or live charts stall)
  location /api/ {
    proxy_pass http://api:8000;
    proxy_http_version 1.1;
    proxy_set_header Connection "";
    proxy_buffering off;
    proxy_read_timeout 1h;
  }

  location /ingest {
    proxy_pass http://api:8000;
  }
}
```

### Linux `systemd` unit for the agent (`/etc/systemd/system/meridian-agent.service`)

```ini
[Unit]
Description=Meridian Core agent
After=network-online.target

[Service]
EnvironmentFile=/etc/meridian/agent.env
ExecStart=/opt/meridian/meridian-agent
Restart=always
RestartSec=5
User=meridian

[Install]
WantedBy=multi-user.target
```

### `.gitignore`

```
.env
agent/build/
api/.venv/
__pycache__/
dashboard/node_modules/
dashboard/dist/
```

---

## 22. Appendix B: first-run checklist

1. Install the prerequisites for your OS (section 4) and note your GPU vendor.
2. `git clone` the repo, then `cp .env.example .env` and set `DB_PASSWORD` and `API_KEY`.
3. `docker compose up -d --build`, then open `http://localhost:8000/health` (expect OK) and `http://localhost:8080` (dashboard).
4. Confirm the database policies exist: `docker compose exec db psql -U postgres -d meridian -c "SELECT job_id, proc_name, hypertable_name FROM timescaledb_information.jobs;"`
5. Build the agent (section 8.8), set `API_URL`, `API_KEY`, `HOST_NAME`, and run it.
6. Within about 15 seconds the dashboard should show a host card with live CPU and GPU values.
7. Create the starter alert rules (section 11.2), run a CPU stress test for 5+ minutes, and confirm one "firing" then one "resolved" notification.
8. Install the agent as a service (section 8.9) so it survives reboots.

---

*Project name "Meridian Core" is a placeholder; rename freely.*
