-- Plain-PostgreSQL stand-in for db/init.sql, used when TimescaleDB is unavailable.
-- time_bucket is shimmed with date_bin; rollups are ordinary views.
DROP VIEW IF EXISTS metrics_1m, metrics_1h;
DROP TABLE IF EXISTS alert_events, alert_rules, metrics CASCADE;
CREATE OR REPLACE FUNCTION time_bucket(interval, timestamptz) RETURNS timestamptz
  LANGUAGE sql IMMUTABLE AS $$ SELECT date_bin($1, $2, timestamptz 'epoch') $$;
CREATE TABLE metrics (time timestamptz NOT NULL, host text NOT NULL, metric text NOT NULL,
  device text NOT NULL DEFAULT '', value double precision NOT NULL);
CREATE VIEW metrics_1m AS SELECT time_bucket('1 minute', time) AS bucket, host, metric, device,
  avg(value) AS avg, min(value) AS min, max(value) AS max FROM metrics GROUP BY 1,2,3,4;
CREATE VIEW metrics_1h AS SELECT time_bucket('1 hour', time) AS bucket, host, metric, device,
  avg(value) AS avg, min(value) AS min, max(value) AS max FROM metrics GROUP BY 1,2,3,4;
CREATE TABLE alert_rules (id serial PRIMARY KEY, name text NOT NULL, host text, metric text NOT NULL,
  device text, op text NOT NULL CHECK (op IN ('>','>=','<','<=')), threshold double precision NOT NULL,
  for_seconds integer NOT NULL DEFAULT 120, severity text NOT NULL DEFAULT 'warning',
  notify_url text, enabled boolean NOT NULL DEFAULT true);
CREATE TABLE alert_events (id bigserial PRIMARY KEY, rule_id integer REFERENCES alert_rules(id) ON DELETE CASCADE,
  host text NOT NULL, device text NOT NULL DEFAULT '', state text NOT NULL CHECK (state IN ('firing','resolved')),
  value double precision, at timestamptz NOT NULL DEFAULT now());
