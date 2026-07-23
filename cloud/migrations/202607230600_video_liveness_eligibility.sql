PRAGMA foreign_keys = ON;

-- Repair legacy rows so the base liveness cursor can use the active-video index
-- without scanning entries that are already excluded by block/death lists.
UPDATE videos
   SET status = 'hidden'
 WHERE status = 'active'
   AND EXISTS (
         SELECT 1
           FROM video_blocklist AS blocked
          WHERE blocked.canonical_key = videos.canonical_key
       )
   AND NOT EXISTS (
         SELECT 1
           FROM video_death_list AS death
          WHERE death.canonical_key = videos.canonical_key
       );

UPDATE videos
   SET status = 'dead'
 WHERE status <> 'dead'
   AND EXISTS (
         SELECT 1
           FROM video_death_list AS death
          WHERE death.canonical_key = videos.canonical_key
       );

-- Replace the separate block-count and video-status triggers with one ordered
-- trigger body. This avoids relying on SQLite's trigger execution order.
DROP TRIGGER IF EXISTS status_counts_dirty_on_block_insert;
DROP TRIGGER IF EXISTS status_counts_delta_on_block_insert;
DROP TRIGGER IF EXISTS status_counts_dirty_on_block_delete;
DROP TRIGGER IF EXISTS status_counts_delta_on_block_delete;
DROP TRIGGER IF EXISTS video_status_hidden_on_block_insert;
DROP TRIGGER IF EXISTS video_status_restore_on_block_delete;
DROP TRIGGER IF EXISTS video_status_dead_on_death_insert;
DROP TRIGGER IF EXISTS video_status_restore_on_death_delete;

CREATE TRIGGER status_counts_delta_on_block_insert
AFTER INSERT ON video_blocklist
BEGIN
  UPDATE status_counts
     SET active_videos = MAX(0, active_videos - EXISTS (
           SELECT 1 FROM videos AS video
            WHERE video.id = NEW.video_id AND video.status = 'active'
         )),
         active_mp4_videos = MAX(0, active_mp4_videos - EXISTS (
           SELECT 1 FROM videos AS video
            WHERE video.id = NEW.video_id
              AND video.status = 'active'
              AND video.media_type = 'mp4'
         )),
         feed_videos = MAX(0, feed_videos - EXISTS (
           SELECT 1 FROM ranking_entries AS ranking
            WHERE ranking.period = '24h' AND ranking.video_id = NEW.video_id
         )),
         feed_mp4_videos = MAX(0, feed_mp4_videos - EXISTS (
           SELECT 1
             FROM ranking_entries AS ranking
             INNER JOIN videos AS video ON video.id = ranking.video_id
            WHERE ranking.period = '24h'
              AND ranking.video_id = NEW.video_id
              AND video.media_type = 'mp4'
         )),
         blocked_videos = blocked_videos + 1,
         updated_at = NEW.blocked_at
   WHERE id = 1;

  UPDATE videos
     SET status = 'hidden'
   WHERE canonical_key = NEW.canonical_key
     AND status = 'active';
END;

CREATE TRIGGER status_counts_delta_on_block_delete
AFTER DELETE ON video_blocklist
BEGIN
  UPDATE status_counts
     SET active_videos = active_videos + EXISTS (
           SELECT 1 FROM videos AS video
            WHERE video.id = OLD.video_id
              AND video.status = 'hidden'
              AND NOT EXISTS (
                    SELECT 1 FROM video_death_list AS death
                     WHERE death.canonical_key = OLD.canonical_key
                  )
         ),
         active_mp4_videos = active_mp4_videos + EXISTS (
           SELECT 1 FROM videos AS video
            WHERE video.id = OLD.video_id
              AND video.status = 'hidden'
              AND video.media_type = 'mp4'
              AND NOT EXISTS (
                    SELECT 1 FROM video_death_list AS death
                     WHERE death.canonical_key = OLD.canonical_key
                  )
         ),
         feed_videos = feed_videos + EXISTS (
           SELECT 1 FROM ranking_entries AS ranking
           INNER JOIN videos AS video ON video.id = ranking.video_id
            WHERE ranking.period = '24h'
              AND ranking.video_id = OLD.video_id
              AND video.status = 'hidden'
              AND NOT EXISTS (
                    SELECT 1 FROM video_death_list AS death
                     WHERE death.canonical_key = OLD.canonical_key
                  )
         ),
         feed_mp4_videos = feed_mp4_videos + EXISTS (
           SELECT 1 FROM ranking_entries AS ranking
           INNER JOIN videos AS video ON video.id = ranking.video_id
            WHERE ranking.period = '24h'
              AND ranking.video_id = OLD.video_id
              AND video.status = 'hidden'
              AND video.media_type = 'mp4'
              AND NOT EXISTS (
                    SELECT 1 FROM video_death_list AS death
                     WHERE death.canonical_key = OLD.canonical_key
                  )
         ),
         blocked_videos = MAX(0, blocked_videos - 1),
         updated_at = CURRENT_TIMESTAMP,
         dirty = 1
   WHERE id = 1;

  UPDATE videos
     SET status = 'active'
   WHERE canonical_key = OLD.canonical_key
     AND status = 'hidden'
     AND NOT EXISTS (
           SELECT 1 FROM video_death_list AS death
            WHERE death.canonical_key = OLD.canonical_key
         );
END;

CREATE TRIGGER video_status_dead_on_death_insert
AFTER INSERT ON video_death_list
BEGIN
  UPDATE videos
     SET status = 'dead'
   WHERE canonical_key = NEW.canonical_key
     AND status <> 'dead';
END;

CREATE TRIGGER video_status_restore_on_death_delete
AFTER DELETE ON video_death_list
BEGIN
  UPDATE videos
     SET status = CASE
       WHEN EXISTS (
         SELECT 1
           FROM video_blocklist AS blocked
          WHERE blocked.canonical_key = OLD.canonical_key
       ) THEN 'hidden'
       ELSE 'active'
     END
   WHERE canonical_key = OLD.canonical_key
     AND status = 'dead';
END;

INSERT OR REPLACE INTO schema_meta(key, value)
VALUES ('schema_version', '20260723-video-liveness-eligibility');
