import { PLAYBACK_FEED_LIMIT } from './feed-limits.js';
import { recentFeedCutoff } from './source-feed-time.js';

const PERIOD = '24h';
const DESIRED_FEED_CTE = `WITH desired AS (
  SELECT video.id AS videoId,
         ROW_NUMBER() OVER (
           ORDER BY video.last_seen_at DESC, video.id DESC
         ) AS desiredRank
    FROM videos AS video
   WHERE video.status = 'active'
     AND video.last_seen_at >= ?
   ORDER BY video.last_seen_at DESC, video.id DESC
   LIMIT ?
)`;

export function deleteStaleCompactedFeedStatement(db, capturedAt) {
  return db.prepare(
    `${DESIRED_FEED_CTE}
     DELETE FROM ranking_entries
      WHERE period = ?
        AND NOT EXISTS (
          SELECT 1 FROM desired
           WHERE desired.videoId = ranking_entries.video_id
        )`
  ).bind(recentFeedCutoff(capturedAt), PLAYBACK_FEED_LIMIT, PERIOD);
}

export function parkMovedCompactedFeedStatement(db, capturedAt) {
  return db.prepare(
    `${DESIRED_FEED_CTE}
     UPDATE ranking_entries
        SET rank = -video_id
      WHERE period = ?
        AND EXISTS (
          SELECT 1 FROM desired
           WHERE desired.videoId = ranking_entries.video_id
             AND desired.desiredRank <> ranking_entries.rank
        )`
  ).bind(recentFeedCutoff(capturedAt), PLAYBACK_FEED_LIMIT, PERIOD);
}

export function upsertDesiredCompactedFeedStatement(db, capturedAt) {
  return db.prepare(
    `${DESIRED_FEED_CTE}
     INSERT INTO ranking_entries (period, video_id, rank, captured_at)
     SELECT ?, desired.videoId, desired.desiredRank, ?
       FROM desired
      WHERE 1
     ON CONFLICT(period, video_id) DO UPDATE SET
       rank = excluded.rank,
       captured_at = excluded.captured_at
     WHERE ranking_entries.rank <> excluded.rank`
  ).bind(
    recentFeedCutoff(capturedAt),
    PLAYBACK_FEED_LIMIT,
    PERIOD,
    capturedAt
  );
}

export function compactedFeedSignatureStatement(db) {
  return db.prepare(
    `SELECT COUNT(*) AS rowCount,
            COALESCE(json_group_array(CAST(video_id AS TEXT)), '[]') AS contentJson
       FROM (
         SELECT video_id
           FROM ranking_entries
          WHERE period = ?
          ORDER BY rank
       )`
  ).bind(PERIOD);
}

export async function syncCompactedFeedInDatabase(db, capturedAt) {
  const results = await db.batch([
    deleteStaleCompactedFeedStatement(db, capturedAt),
    parkMovedCompactedFeedStatement(db, capturedAt),
    upsertDesiredCompactedFeedStatement(db, capturedAt),
    compactedFeedSignatureStatement(db)
  ]);
  const signature = results?.[3]?.results?.[0] || {};
  return {
    rowCount: Math.max(0, Number(signature.rowCount || 0)),
    contentJson: String(signature.contentJson || '[]')
  };
}
