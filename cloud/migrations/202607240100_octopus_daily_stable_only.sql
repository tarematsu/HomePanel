PRAGMA foreign_keys = ON;

-- Octopus has one fixed electricity supply point. Keep only complete JST daily
-- totals and use the composite primary key itself for range reads.
CREATE TABLE octopus_daily_totals_v2 (
  account_number TEXT NOT NULL,
  day TEXT NOT NULL,
  energy_kwh REAL NOT NULL CHECK(energy_kwh >= 0),
  slot_count INTEGER NOT NULL CHECK(slot_count = 48),
  updated_at INTEGER NOT NULL,
  PRIMARY KEY(account_number, day)
) WITHOUT ROWID;

INSERT INTO octopus_daily_totals_v2(account_number,day,energy_kwh,slot_count,updated_at)
SELECT account_number,
       day,
       SUM(energy_kwh),
       SUM(slot_count),
       MAX(updated_at)
  FROM octopus_daily_totals
 GROUP BY account_number,day
HAVING SUM(slot_count) = 48;

DROP TABLE octopus_daily_totals;
ALTER TABLE octopus_daily_totals_v2 RENAME TO octopus_daily_totals;

-- Half-hour rows and the legacy backfill/range markers are no longer used.
DROP TABLE IF EXISTS octopus_readings;
DROP TABLE IF EXISTS octopus_backfill_state;
DROP TABLE IF EXISTS octopus_sync_ranges;

CREATE TABLE octopus_sync_state (
  account_number TEXT PRIMARY KEY,
  stable_through INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
) WITHOUT ROWID;

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
VALUES ('schema_version', '20260724-octopus-daily-source-of-truth');
