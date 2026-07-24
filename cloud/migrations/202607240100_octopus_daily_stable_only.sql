-- Run Octopus collection once per day. Preserve an already queued immediate or
-- earlier refresh; scheduler reconciliation applies the same interval at runtime.
UPDATE jobs
   SET interval_seconds = 86400,
       next_run_at = CASE
         WHEN next_run_at = 0 THEN 0
         ELSE MIN(next_run_at, unixepoch() + 86400)
       END
 WHERE name = 'octopus';

INSERT OR REPLACE INTO schema_meta(key, value)
VALUES ('schema_version', '20260724-octopus-daily-stable-only');
