-- Monitor the Stationhead collector independently from playback ingestion.
INSERT OR IGNORE INTO jobs(name, interval_seconds, next_run_at)
VALUES ('stationhead_health', 300, 0);
