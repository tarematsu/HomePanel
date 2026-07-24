PRAGMA foreign_keys = ON;

-- Ignore current_state sources that do not participate in device sync. The
-- previous generic triggers rewrote the singleton manifest for update metadata
-- and any future internal source even though every projected value was unchanged.
DROP TRIGGER IF EXISTS sync_manifest_on_state_insert;
DROP TRIGGER IF EXISTS sync_manifest_on_state_update;
DROP TRIGGER IF EXISTS sync_manifest_on_state_delete;

CREATE TRIGGER sync_manifest_on_state_insert
AFTER INSERT ON current_state
WHEN NEW.source IN (
  'weather','news','octopus','switchbot','stationhead','environment',
  'radar','stationhead_health'
)
BEGIN
  UPDATE sync_manifest
     SET dashboard_version=dashboard_version + CASE WHEN NEW.source IN (
           'weather','news','octopus','switchbot','stationhead','environment'
         ) THEN NEW.version ELSE 0 END,
         environment_version = CASE WHEN NEW.source='environment' THEN NEW.version ELSE environment_version END,
         environment_fetched_at = CASE WHEN NEW.source='environment' THEN NEW.fetched_at ELSE environment_fetched_at END,
         radar_version = CASE WHEN NEW.source='radar' THEN NEW.version ELSE radar_version END,
         switchbot_version = CASE WHEN NEW.source='switchbot' THEN NEW.version ELSE switchbot_version END,
         stationhead_version = CASE WHEN NEW.source='stationhead' THEN NEW.version ELSE stationhead_version END,
         stationhead_health_version = CASE WHEN NEW.source='stationhead_health' THEN NEW.version ELSE stationhead_health_version END,
         updated_at=MAX(updated_at,NEW.fetched_at)
   WHERE id=1;
END;

CREATE TRIGGER sync_manifest_on_state_update
AFTER UPDATE OF version,fetched_at ON current_state
WHEN NEW.source IN (
       'weather','news','octopus','switchbot','stationhead','environment',
       'radar','stationhead_health'
     )
 AND (
       NEW.version<>OLD.version
       OR (NEW.source='environment' AND NEW.fetched_at<>OLD.fetched_at)
     )
BEGIN
  UPDATE sync_manifest
     SET dashboard_version=dashboard_version + CASE WHEN NEW.source IN (
           'weather','news','octopus','switchbot','stationhead','environment'
         ) THEN NEW.version-OLD.version ELSE 0 END,
         environment_version = CASE WHEN NEW.source='environment' THEN NEW.version ELSE environment_version END,
         environment_fetched_at = CASE WHEN NEW.source='environment' THEN NEW.fetched_at ELSE environment_fetched_at END,
         radar_version = CASE WHEN NEW.source='radar' THEN NEW.version ELSE radar_version END,
         switchbot_version = CASE WHEN NEW.source='switchbot' THEN NEW.version ELSE switchbot_version END,
         stationhead_version = CASE WHEN NEW.source='stationhead' THEN NEW.version ELSE stationhead_version END,
         stationhead_health_version = CASE WHEN NEW.source='stationhead_health' THEN NEW.version ELSE stationhead_health_version END,
         updated_at=MAX(updated_at,NEW.fetched_at)
   WHERE id=1;
END;

CREATE TRIGGER sync_manifest_on_state_delete
AFTER DELETE ON current_state
WHEN OLD.source IN (
  'weather','news','octopus','switchbot','stationhead','environment',
  'radar','stationhead_health'
)
BEGIN
  UPDATE sync_manifest
     SET dashboard_version=MAX(0,dashboard_version - CASE WHEN OLD.source IN (
           'weather','news','octopus','switchbot','stationhead','environment'
         ) THEN OLD.version ELSE 0 END),
         environment_version = CASE WHEN OLD.source='environment' THEN 0 ELSE environment_version END,
         environment_fetched_at = CASE WHEN OLD.source='environment' THEN 0 ELSE environment_fetched_at END,
         radar_version = CASE WHEN OLD.source='radar' THEN 0 ELSE radar_version END,
         switchbot_version = CASE WHEN OLD.source='switchbot' THEN 0 ELSE switchbot_version END,
         stationhead_version = CASE WHEN OLD.source='stationhead' THEN 0 ELSE stationhead_version END,
         stationhead_health_version = CASE WHEN OLD.source='stationhead_health' THEN 0 ELSE stationhead_health_version END
   WHERE id=1;
END;

-- Only mutate status_counts when a row contributes to a maintained count.
-- Video deletion is handled BEFORE the parent row disappears so its 24h
-- ranking contribution is still observable; cascade ranking deletes then see
-- no active parent and deliberately perform no second decrement.
DROP TRIGGER IF EXISTS status_counts_on_video_insert;
DROP TRIGGER IF EXISTS status_counts_on_video_delete;
DROP TRIGGER IF EXISTS status_counts_on_video_update;
DROP TRIGGER IF EXISTS status_counts_on_ranking_insert;
DROP TRIGGER IF EXISTS status_counts_on_ranking_delete;

CREATE TRIGGER status_counts_on_video_insert
AFTER INSERT ON videos
WHEN NEW.status='active'
BEGIN
  UPDATE status_counts
     SET active_videos=active_videos+1,
         active_mp4_videos=active_mp4_videos+(NEW.media_type='mp4'),
         updated_at=CURRENT_TIMESTAMP,
         dirty=0
   WHERE id=1;
END;

CREATE TRIGGER status_counts_on_video_delete
BEFORE DELETE ON videos
WHEN OLD.status='active'
BEGIN
  UPDATE status_counts
     SET active_videos=MAX(0,active_videos-1),
         active_mp4_videos=MAX(0,active_mp4_videos-(OLD.media_type='mp4')),
         feed_videos=MAX(0,feed_videos-EXISTS(
           SELECT 1 FROM ranking_entries
            WHERE period='24h' AND video_id=OLD.id
         )),
         feed_mp4_videos=MAX(0,feed_mp4_videos-
           (OLD.media_type='mp4')*EXISTS(
             SELECT 1 FROM ranking_entries
              WHERE period='24h' AND video_id=OLD.id
           )),
         updated_at=CURRENT_TIMESTAMP,
         dirty=0
   WHERE id=1;
END;

CREATE TRIGGER status_counts_on_video_update
AFTER UPDATE OF status,media_type ON videos
WHEN (NEW.status='active')<>(OLD.status='active')
  OR (NEW.status='active' AND NEW.media_type='mp4')<>
     (OLD.status='active' AND OLD.media_type='mp4')
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
 AND EXISTS(SELECT 1 FROM videos WHERE id=NEW.video_id AND status='active')
BEGIN
  UPDATE status_counts
     SET feed_videos=feed_videos+1,
         feed_mp4_videos=feed_mp4_videos+EXISTS(
           SELECT 1 FROM videos
            WHERE id=NEW.video_id AND status='active' AND media_type='mp4'
         ),
         updated_at=CURRENT_TIMESTAMP,
         dirty=0
   WHERE id=1;
END;

CREATE TRIGGER status_counts_on_ranking_delete
AFTER DELETE ON ranking_entries
WHEN OLD.period='24h'
 AND EXISTS(SELECT 1 FROM videos WHERE id=OLD.video_id AND status='active')
BEGIN
  UPDATE status_counts
     SET feed_videos=MAX(0,feed_videos-1),
         feed_mp4_videos=MAX(0,feed_mp4_videos-EXISTS(
           SELECT 1 FROM videos
            WHERE id=OLD.video_id AND status='active' AND media_type='mp4'
         )),
         updated_at=CURRENT_TIMESTAMP,
         dirty=0
   WHERE id=1;
END;

-- Repair any count drift created by parent deletes before this migration.
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
VALUES('schema_version','20260724-d1-runtime-bugfixes');
