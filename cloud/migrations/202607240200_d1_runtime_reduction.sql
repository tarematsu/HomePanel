PRAGMA foreign_keys = ON;

-- High-frequency text primary keys do not need a duplicate rowid b-tree.
CREATE TABLE jobs_v2 (
  name TEXT PRIMARY KEY,
  interval_seconds INTEGER NOT NULL,
  next_run_at INTEGER NOT NULL,
  lease_until INTEGER,
  last_success_at INTEGER,
  last_error TEXT,
  consecutive_failures INTEGER NOT NULL DEFAULT 0
) WITHOUT ROWID;
INSERT INTO jobs_v2
SELECT name,interval_seconds,next_run_at,lease_until,last_success_at,last_error,consecutive_failures
  FROM jobs;
DROP TABLE jobs;
ALTER TABLE jobs_v2 RENAME TO jobs;

CREATE TABLE current_state_v2 (
  source TEXT PRIMARY KEY,
  version INTEGER NOT NULL,
  payload TEXT NOT NULL,
  observed_at INTEGER,
  fetched_at INTEGER NOT NULL,
  last_success_at INTEGER,
  status TEXT NOT NULL CHECK (status IN ('ok','stale','error')),
  error TEXT,
  content_hash TEXT
) WITHOUT ROWID;
INSERT INTO current_state_v2
SELECT source,version,payload,observed_at,fetched_at,last_success_at,status,error,content_hash
  FROM current_state;
DROP TABLE current_state;
ALTER TABLE current_state_v2 RENAME TO current_state;

CREATE TABLE device_heartbeats_v2 (
  device_id TEXT PRIMARY KEY,
  last_seen_at INTEGER NOT NULL,
  app_version TEXT,
  stationhead_ok INTEGER,
  outbox_count INTEGER,
  payload TEXT,
  last_sequence INTEGER NOT NULL DEFAULT 0
) WITHOUT ROWID;
INSERT INTO device_heartbeats_v2
SELECT device_id,last_seen_at,app_version,stationhead_ok,outbox_count,payload,last_sequence
  FROM device_heartbeats;
DROP TABLE device_heartbeats;
ALTER TABLE device_heartbeats_v2 RENAME TO device_heartbeats;

-- Device sync reads this single row instead of aggregating current_state on every poll.
CREATE TABLE sync_manifest (
  id INTEGER PRIMARY KEY CHECK(id=1),
  dashboard_version INTEGER NOT NULL DEFAULT 0,
  environment_version INTEGER NOT NULL DEFAULT 0,
  environment_fetched_at INTEGER NOT NULL DEFAULT 0,
  radar_version INTEGER NOT NULL DEFAULT 0,
  switchbot_version INTEGER NOT NULL DEFAULT 0,
  stationhead_version INTEGER NOT NULL DEFAULT 0,
  stationhead_health_version INTEGER NOT NULL DEFAULT 0,
  updated_at INTEGER NOT NULL DEFAULT 0
) WITHOUT ROWID;

INSERT INTO sync_manifest(
  id,dashboard_version,environment_version,environment_fetched_at,
  radar_version,switchbot_version,stationhead_version,
  stationhead_health_version,updated_at
)
SELECT 1,
       COALESCE(SUM(CASE WHEN source IN (
         'weather','news','octopus','switchbot','stationhead','environment'
       ) THEN version ELSE 0 END),0),
       COALESCE(MAX(CASE WHEN source='environment' THEN version ELSE 0 END),0),
       COALESCE(MAX(CASE WHEN source='environment' THEN fetched_at ELSE 0 END),0),
       COALESCE(MAX(CASE WHEN source='radar' THEN version ELSE 0 END),0),
       COALESCE(MAX(CASE WHEN source='switchbot' THEN version ELSE 0 END),0),
       COALESCE(MAX(CASE WHEN source='stationhead' THEN version ELSE 0 END),0),
       COALESCE(MAX(CASE WHEN source='stationhead_health' THEN version ELSE 0 END),0),
       COALESCE(MAX(fetched_at),0)
  FROM current_state;

CREATE TRIGGER sync_manifest_on_state_insert
AFTER INSERT ON current_state
BEGIN
  UPDATE sync_manifest
     SET dashboard_version=dashboard_version + CASE WHEN NEW.source IN (
           'weather','news','octopus','switchbot','stationhead','environment'
         ) THEN NEW.version ELSE 0 END,
         environment_version=CASE WHEN NEW.source='environment' THEN NEW.version ELSE environment_version END,
         environment_fetched_at=CASE WHEN NEW.source='environment' THEN NEW.fetched_at ELSE environment_fetched_at END,
         radar_version=CASE WHEN NEW.source='radar' THEN NEW.version ELSE radar_version END,
         switchbot_version=CASE WHEN NEW.source='switchbot' THEN NEW.version ELSE switchbot_version END,
         stationhead_version=CASE WHEN NEW.source='stationhead' THEN NEW.version ELSE stationhead_version END,
         stationhead_health_version=CASE WHEN NEW.source='stationhead_health' THEN NEW.version ELSE stationhead_health_version END,
         updated_at=MAX(updated_at,NEW.fetched_at)
   WHERE id=1;
END;

CREATE TRIGGER sync_manifest_on_state_update
AFTER UPDATE OF version,fetched_at ON current_state
WHEN NEW.version<>OLD.version
  OR (NEW.source='environment' AND NEW.fetched_at<>OLD.fetched_at)
BEGIN
  UPDATE sync_manifest
     SET dashboard_version=dashboard_version + CASE WHEN NEW.source IN (
           'weather','news','octopus','switchbot','stationhead','environment'
         ) THEN NEW.version-OLD.version ELSE 0 END,
         environment_version=CASE WHEN NEW.source='environment' THEN NEW.version ELSE environment_version END,
         environment_fetched_at=CASE WHEN NEW.source='environment' THEN NEW.fetched_at ELSE environment_fetched_at END,
         radar_version=CASE WHEN NEW.source='radar' THEN NEW.version ELSE radar_version END,
         switchbot_version=CASE WHEN NEW.source='switchbot' THEN NEW.version ELSE switchbot_version END,
         stationhead_version=CASE WHEN NEW.source='stationhead' THEN NEW.version ELSE stationhead_version END,
         stationhead_health_version=CASE WHEN NEW.source='stationhead_health' THEN NEW.version ELSE stationhead_health_version END,
         updated_at=MAX(updated_at,NEW.fetched_at)
   WHERE id=1;
END;

CREATE TRIGGER sync_manifest_on_state_delete
AFTER DELETE ON current_state
BEGIN
  UPDATE sync_manifest
     SET dashboard_version=MAX(0,dashboard_version - CASE WHEN OLD.source IN (
           'weather','news','octopus','switchbot','stationhead','environment'
         ) THEN OLD.version ELSE 0 END),
         environment_version=CASE WHEN OLD.source='environment' THEN 0 ELSE environment_version END,
         environment_fetched_at=CASE WHEN OLD.source='environment' THEN 0 ELSE environment_fetched_at END,
         radar_version=CASE WHEN OLD.source='radar' THEN 0 ELSE radar_version END,
         switchbot_version=CASE WHEN OLD.source='switchbot' THEN 0 ELSE switchbot_version END,
         stationhead_version=CASE WHEN OLD.source='stationhead' THEN 0 ELSE stationhead_version END,
         stationhead_health_version=CASE WHEN OLD.source='stationhead_health' THEN 0 ELSE stationhead_health_version END
   WHERE id=1;
END;

CREATE INDEX IF NOT EXISTS idx_device_commands_pending_delivery
  ON device_commands(device_id,command,delivered_at,expires_at,id)
  WHERE completed_at IS NULL;

-- Successful scheduler runs live only in Durable Object storage. Persist only
-- failure and recovery transitions for operator history.
CREATE TABLE job_events (
  job_name TEXT NOT NULL,
  occurred_at INTEGER NOT NULL,
  event TEXT NOT NULL CHECK(event IN ('failed','recovered')),
  detail TEXT,
  PRIMARY KEY(job_name,occurred_at,event)
) WITHOUT ROWID;

-- Compact telemetry is the sole supported path; history is canonical in R2.
DROP TABLE IF EXISTS environment_samples;
DROP TABLE IF EXISTS environment_buckets;

UPDATE jobs
   SET interval_seconds=3600,
       next_run_at=CASE
         WHEN next_run_at=0 THEN 0
         ELSE MIN(next_run_at,unixepoch()+3600)
       END
 WHERE name='video_liveness';

-- Replace dirty/full-recount behavior with exact deltas for all mutations.
DROP TRIGGER IF EXISTS status_counts_dirty_on_block_insert;
DROP TRIGGER IF EXISTS status_counts_dirty_on_block_delete;
DROP TRIGGER IF EXISTS status_counts_delta_on_block_insert;
DROP TRIGGER IF EXISTS status_counts_delta_on_block_delete;
DROP TRIGGER IF EXISTS status_counts_dirty_on_death_insert;
DROP TRIGGER IF EXISTS status_counts_dirty_on_death_delete;
DROP TRIGGER IF EXISTS video_status_hidden_on_block_insert;
DROP TRIGGER IF EXISTS video_status_restore_on_block_delete;
DROP TRIGGER IF EXISTS video_status_dead_on_death_insert;
DROP TRIGGER IF EXISTS video_status_restore_on_death_delete;
DROP TRIGGER IF EXISTS video_death_keep_status;
DROP TRIGGER IF EXISTS status_counts_on_video_insert;
DROP TRIGGER IF EXISTS status_counts_on_video_delete;
DROP TRIGGER IF EXISTS status_counts_on_video_update;
DROP TRIGGER IF EXISTS status_counts_on_ranking_insert;
DROP TRIGGER IF EXISTS status_counts_on_ranking_delete;
DROP TRIGGER IF EXISTS status_counts_on_block_insert;
DROP TRIGGER IF EXISTS status_counts_on_block_delete;
DROP TRIGGER IF EXISTS status_counts_on_death_insert;
DROP TRIGGER IF EXISTS status_counts_on_death_delete;

CREATE TRIGGER status_counts_on_video_insert
AFTER INSERT ON videos
BEGIN
  UPDATE status_counts
     SET active_videos=active_videos+(NEW.status='active'),
         active_mp4_videos=active_mp4_videos+(NEW.status='active' AND NEW.media_type='mp4'),
         updated_at=CURRENT_TIMESTAMP,
         dirty=0
   WHERE id=1;
END;

CREATE TRIGGER status_counts_on_video_delete
AFTER DELETE ON videos
BEGIN
  UPDATE status_counts
     SET active_videos=MAX(0,active_videos-(OLD.status='active')),
         active_mp4_videos=MAX(0,active_mp4_videos-(OLD.status='active' AND OLD.media_type='mp4')),
         updated_at=CURRENT_TIMESTAMP,
         dirty=0
   WHERE id=1;
END;

CREATE TRIGGER status_counts_on_video_update
AFTER UPDATE OF status,media_type ON videos
WHEN OLD.status IS NOT NEW.status OR OLD.media_type IS NOT NEW.media_type
BEGIN
  UPDATE status_counts
     SET active_videos=MAX(0,active_videos+(NEW.status='active')-(OLD.status='active')),
         active_mp4_videos=MAX(0,active_mp4_videos+
           (NEW.status='active' AND NEW.media_type='mp4')-
           (OLD.status='active' AND OLD.media_type='mp4')),
         feed_videos=MAX(0,feed_videos+
           ((NEW.status='active')-(OLD.status='active'))*EXISTS(
             SELECT 1 FROM ranking_entries
              WHERE period='24h' AND video_id=NEW.id
           )),
         feed_mp4_videos=MAX(0,feed_mp4_videos+
           ((NEW.status='active' AND NEW.media_type='mp4')-
            (OLD.status='active' AND OLD.media_type='mp4'))*EXISTS(
             SELECT 1 FROM ranking_entries
              WHERE period='24h' AND video_id=NEW.id
           )),
         updated_at=CURRENT_TIMESTAMP,
         dirty=0
   WHERE id=1;
END;

CREATE TRIGGER status_counts_on_ranking_insert
AFTER INSERT ON ranking_entries
WHEN NEW.period='24h'
BEGIN
  UPDATE status_counts
     SET feed_videos=feed_videos+EXISTS(
           SELECT 1 FROM videos WHERE id=NEW.video_id AND status='active'
         ),
         feed_mp4_videos=feed_mp4_videos+EXISTS(
           SELECT 1 FROM videos WHERE id=NEW.video_id AND status='active' AND media_type='mp4'
         ),
         updated_at=CURRENT_TIMESTAMP,
         dirty=0
   WHERE id=1;
END;

CREATE TRIGGER status_counts_on_ranking_delete
AFTER DELETE ON ranking_entries
WHEN OLD.period='24h'
BEGIN
  UPDATE status_counts
     SET feed_videos=MAX(0,feed_videos-EXISTS(
           SELECT 1 FROM videos WHERE id=OLD.video_id AND status='active'
         )),
         feed_mp4_videos=MAX(0,feed_mp4_videos-EXISTS(
           SELECT 1 FROM videos WHERE id=OLD.video_id AND status='active' AND media_type='mp4'
         )),
         updated_at=CURRENT_TIMESTAMP,
         dirty=0
   WHERE id=1;
END;

CREATE TRIGGER status_counts_on_block_insert
AFTER INSERT ON video_blocklist
BEGIN
  UPDATE status_counts
     SET blocked_videos=blocked_videos+1,
         updated_at=NEW.blocked_at,
         dirty=0
   WHERE id=1;
  UPDATE videos
     SET status='hidden'
   WHERE canonical_key=NEW.canonical_key AND status='active';
END;

CREATE TRIGGER status_counts_on_block_delete
AFTER DELETE ON video_blocklist
BEGIN
  UPDATE status_counts
     SET blocked_videos=MAX(0,blocked_videos-1),
         updated_at=CURRENT_TIMESTAMP,
         dirty=0
   WHERE id=1;
  UPDATE videos
     SET status=CASE WHEN EXISTS(
       SELECT 1 FROM video_death_list WHERE canonical_key=OLD.canonical_key
     ) THEN 'dead' ELSE 'active' END
   WHERE canonical_key=OLD.canonical_key AND status='hidden';
END;

CREATE TRIGGER status_counts_on_death_insert
AFTER INSERT ON video_death_list
BEGIN
  UPDATE status_counts
     SET death_videos=death_videos+1,
         updated_at=NEW.detected_at,
         dirty=0
   WHERE id=1;
  UPDATE videos
     SET status='dead'
   WHERE canonical_key=NEW.canonical_key AND status<>'dead';
END;

CREATE TRIGGER status_counts_on_death_delete
AFTER DELETE ON video_death_list
BEGIN
  UPDATE status_counts
     SET death_videos=MAX(0,death_videos-1),
         updated_at=CURRENT_TIMESTAMP,
         dirty=0
   WHERE id=1;
  UPDATE videos
     SET status=CASE WHEN EXISTS(
       SELECT 1 FROM video_blocklist WHERE canonical_key=OLD.canonical_key
     ) THEN 'hidden' ELSE 'active' END
   WHERE canonical_key=OLD.canonical_key AND status='dead';
END;

UPDATE status_counts
   SET active_videos=(SELECT COUNT(*) FROM videos WHERE status='active'),
       active_mp4_videos=(SELECT COUNT(*) FROM videos WHERE status='active' AND media_type='mp4'),
       feed_videos=(
         SELECT COUNT(*) FROM ranking_entries AS ranking
         INNER JOIN videos AS video ON video.id=ranking.video_id
         WHERE ranking.period='24h' AND video.status='active'
       ),
       feed_mp4_videos=(
         SELECT COUNT(*) FROM ranking_entries AS ranking
         INNER JOIN videos AS video ON video.id=ranking.video_id
         WHERE ranking.period='24h' AND video.status='active' AND video.media_type='mp4'
       ),
       blocked_videos=(SELECT COUNT(*) FROM video_blocklist),
       death_videos=(SELECT COUNT(*) FROM video_death_list),
       updated_at=CURRENT_TIMESTAMP,
       dirty=0
 WHERE id=1;

INSERT OR REPLACE INTO schema_meta(key,value)
VALUES('schema_version','20260724-d1-runtime-reduction');
