const STATUS_VIDEO_COUNTS_REFRESH = `INSERT INTO status_counts (
  id, active_videos, active_mp4_videos, feed_videos, feed_mp4_videos,
  blocked_videos, death_videos, updated_at
)
WITH active AS (
  SELECT COUNT(*) AS activeVideos,
         COALESCE(SUM(media_type = 'mp4'), 0) AS activeMp4Videos
    FROM videos
   WHERE status = 'active'
), feed AS (
  SELECT COUNT(*) AS feedVideos,
         COALESCE(SUM(video.media_type = 'mp4'), 0) AS feedMp4Videos
    FROM ranking_entries AS ranking
    INNER JOIN videos AS video ON video.id = ranking.video_id
   WHERE ranking.period = '24h'
)
SELECT 1, activeVideos, activeMp4Videos, feedVideos, feedMp4Videos, 0, 0, ?
  FROM active CROSS JOIN feed
 WHERE true
ON CONFLICT(id) DO UPDATE SET
  active_videos=excluded.active_videos,
  active_mp4_videos=excluded.active_mp4_videos,
  feed_videos=excluded.feed_videos,
  feed_mp4_videos=excluded.feed_mp4_videos,
  updated_at=excluded.updated_at`;

const STATUS_EXCLUSION_COUNTS_REFRESH = `INSERT INTO status_counts (
  id, active_videos, active_mp4_videos, feed_videos, feed_mp4_videos,
  blocked_videos, death_videos, updated_at
)
WITH blocked AS (
  SELECT COUNT(*) AS blockedVideos FROM video_blocklist
), death AS (
  SELECT COUNT(*) AS deathVideos FROM video_death_list
)
SELECT 1, 0, 0, 0, 0, blockedVideos, deathVideos, ?
  FROM blocked CROSS JOIN death
 WHERE true
ON CONFLICT(id) DO UPDATE SET
  blocked_videos=excluded.blocked_videos,
  death_videos=excluded.death_videos,
  updated_at=excluded.updated_at`;

const STATUS_COUNTS_CLEAR_DIRTY = `UPDATE status_counts
  SET dirty = 0, updated_at = ?
  WHERE id = 1
  RETURNING active_videos AS activeVideos,
            active_mp4_videos AS activeMp4Videos,
            feed_videos AS feedVideos,
            feed_mp4_videos AS feedMp4Videos,
            blocked_videos AS blockedVideos,
            death_videos AS deathVideos,
            dirty AS countsDirty,
            updated_at AS countsUpdatedAt`;

const STATUS_COUNTS_READ = `SELECT
  active_videos AS activeVideos,
  active_mp4_videos AS activeMp4Videos,
  feed_videos AS feedVideos,
  feed_mp4_videos AS feedMp4Videos,
  blocked_videos AS blockedVideos,
  death_videos AS deathVideos,
  dirty AS countsDirty,
  updated_at AS countsUpdatedAt
FROM status_counts WHERE id = 1`;

function prepareStatusVideoCountsRefresh(db, capturedAt) {
  return db.prepare(STATUS_VIDEO_COUNTS_REFRESH).bind(capturedAt);
}

function prepareStatusExclusionCountsRefresh(db, capturedAt) {
  return db.prepare(STATUS_EXCLUSION_COUNTS_REFRESH).bind(capturedAt);
}

export function emptyStatusCounts() {
  return {
    activeVideos: 0,
    activeMp4Videos: 0,
    feedVideos: 0,
    feedMp4Videos: 0,
    blockedVideos: 0,
    deathVideos: 0,
    countsDirty: 1,
    countsUpdatedAt: null
  };
}

export function prepareStatusCountsRead(db) {
  return db.prepare(STATUS_COUNTS_READ);
}

export async function refreshStatusVideoCounts(db, capturedAt = new Date().toISOString()) {
  await prepareStatusVideoCountsRefresh(db, capturedAt).run();
}

export async function refreshStatusExclusionCounts(db, capturedAt = new Date().toISOString()) {
  await prepareStatusExclusionCountsRefresh(db, capturedAt).run();
}

export async function refreshStatusCounts(db, capturedAt = new Date().toISOString()) {
  const current = await prepareStatusCountsRead(db).first();
  if (current?.countsUpdatedAt && Number(current.countsDirty || 0) === 0) return current;

  const results = await db.batch([
    prepareStatusVideoCountsRefresh(db, capturedAt),
    prepareStatusExclusionCountsRefresh(db, capturedAt),
    db.prepare(STATUS_COUNTS_CLEAR_DIRTY).bind(capturedAt)
  ]);
  return results?.[2]?.results?.[0] || readStatusCounts(db);
}

export async function readStatusCounts(db) {
  return (await prepareStatusCountsRead(db).first()) || emptyStatusCounts();
}

export {
  STATUS_COUNTS_CLEAR_DIRTY,
  STATUS_COUNTS_READ,
  STATUS_EXCLUSION_COUNTS_REFRESH,
  STATUS_VIDEO_COUNTS_REFRESH
};
