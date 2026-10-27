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

-- Keep the aggregates fresh. Every start_offset must stay SHORTER than raw
-- retention (7 days below), otherwise a refresh can wipe aggregate history.
SELECT add_continuous_aggregate_policy('metrics_1m',
    start_offset      => INTERVAL '3 days',
    end_offset        => INTERVAL '1 minute',
    schedule_interval => INTERVAL '1 minute',
    if_not_exists     => TRUE);

SELECT add_continuous_aggregate_policy('metrics_1h',
    start_offset      => INTERVAL '3 days',
    end_offset        => INTERVAL '1 hour',
    schedule_interval => INTERVAL '1 hour',
    if_not_exists     => TRUE);

-- Retention: raw 7 days, 1-minute rollup 90 days, 1-hour rollup 2 years
SELECT add_retention_policy('metrics',    drop_after => INTERVAL '7 days',  if_not_exists => TRUE);
SELECT add_retention_policy('metrics_1m', drop_after => INTERVAL '90 days', if_not_exists => TRUE);
SELECT add_retention_policy('metrics_1h', drop_after => INTERVAL '2 years', if_not_exists => TRUE);

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

CREATE INDEX IF NOT EXISTS alert_events_at ON alert_events (at DESC);

-- Starter rules (section 11.2); only inserted into an empty table.
INSERT INTO alert_rules (name, metric, op, threshold, for_seconds, severity)
SELECT * FROM (VALUES
    ('GPU hot',          'gpu.temp_c',     '>',  85::double precision, 120, 'critical'),
    ('GPU pegged',       'gpu.util',       '>=', 98, 600, 'warning'),
    ('CPU saturated',    'cpu.util_total', '>',  90, 300, 'warning'),
    ('Memory pressure',  'mem.used_pct',   '>',  92, 300, 'warning'),
    ('Disk almost full', 'disk.used_pct',  '>',  90,  60, 'warning')
) AS v(name, metric, op, threshold, for_seconds, severity)
WHERE NOT EXISTS (SELECT 1 FROM alert_rules);
