import { applyD1Migrations, env } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
});

describe("D1 read hotspot migration", () => {
  it("uses sqlite_sequence as the liveness high-water mark without write triggers", async () => {
    const before = await env.DB.prepare(
      "SELECT max_video_id FROM video_liveness_bounds WHERE id=1",
    ).first<{ max_video_id: number }>();
    expect(Number(before?.max_video_id)).toBe(0);

    await env.DB.prepare(
      `INSERT INTO videos(media_url,canonical_key,media_type,first_seen_at,last_seen_at,status)
       VALUES(?1,?2,'mp4',?3,?3,'active')`,
    ).bind("https://media.test/one.mp4", "media.test/one.mp4", "2026-07-23T00:00:00.000Z").run();
    await env.DB.prepare(
      `INSERT INTO videos(media_url,canonical_key,media_type,first_seen_at,last_seen_at,status)
       VALUES(?1,?2,'mp4',?3,?3,'hidden')`,
    ).bind("https://media.test/two.mp4", "media.test/two.mp4", "2026-07-23T00:00:01.000Z").run();

    const after = await env.DB.prepare(
      "SELECT max_video_id FROM video_liveness_bounds WHERE id=1",
    ).first<{ max_video_id: number }>();
    expect(Number(after?.max_video_id)).toBe(2);

    const triggers = await env.DB.prepare(
      "SELECT COUNT(*) AS count FROM sqlite_schema WHERE type='trigger' AND name LIKE 'video_liveness_bound_%'",
    ).first<{ count: number }>();
    expect(Number(triggers?.count)).toBe(0);
  });

  it("restores active counts without marking the exact counter state dirty", async () => {
    const timestamp = "2026-07-23T00:00:00.000Z";
    const inserted = await env.DB.prepare(
      `INSERT INTO videos(media_url,canonical_key,media_type,first_seen_at,last_seen_at,status)
       VALUES(?1,?2,'mp4',?3,?3,'active') RETURNING id`,
    ).bind("https://media.test/blocked.mp4", "media.test/blocked.mp4", timestamp)
      .first<{ id: number }>();
    const videoId = Number(inserted?.id);

    await env.DB.prepare(
      `UPDATE status_counts
          SET active_videos=1,active_mp4_videos=1,feed_videos=0,feed_mp4_videos=0,
              blocked_videos=0,death_videos=0,dirty=0,updated_at=?1
        WHERE id=1`,
    ).bind(timestamp).run();
    await env.DB.prepare(
      `INSERT INTO video_blocklist(canonical_key,media_url,video_id,blocked_at,reason)
       VALUES(?1,?2,?3,?4,'test')`,
    ).bind("media.test/blocked.mp4", "https://media.test/blocked.mp4", videoId, timestamp).run();
    await env.DB.prepare("DELETE FROM video_blocklist WHERE canonical_key=?1")
      .bind("media.test/blocked.mp4").run();

    const counts = await env.DB.prepare(
      "SELECT active_videos,blocked_videos,dirty FROM status_counts WHERE id=1",
    ).first<{ active_videos: number; blocked_videos: number; dirty: number }>();
    expect(counts).toMatchObject({ active_videos: 1, blocked_videos: 0, dirty: 0 });
    const video = await env.DB.prepare("SELECT status FROM videos WHERE id=?1")
      .bind(videoId).first<{ status: string }>();
    expect(video?.status).toBe("active");
  });

  it("decrements active and feed counts before a ranked video cascades away", async () => {
    const timestamp = "2026-07-23T00:00:00.000Z";
    const inserted = await env.DB.prepare(
      `INSERT INTO videos(media_url,canonical_key,media_type,first_seen_at,last_seen_at,status)
       VALUES(?1,?2,'mp4',?3,?3,'active') RETURNING id`,
    ).bind("https://media.test/ranked.mp4", "media.test/ranked.mp4", timestamp)
      .first<{ id: number }>();
    const videoId = Number(inserted?.id);
    await env.DB.prepare(
      `INSERT INTO ranking_entries(period,video_id,rank,captured_at)
       VALUES('24h',?1,1,?2)`,
    ).bind(videoId, timestamp).run();

    const before = await env.DB.prepare(
      `SELECT active_videos,active_mp4_videos,feed_videos,feed_mp4_videos
         FROM status_counts WHERE id=1`,
    ).first<Record<string, number>>();
    expect(before).toMatchObject({
      active_videos: 1,
      active_mp4_videos: 1,
      feed_videos: 1,
      feed_mp4_videos: 1,
    });

    await env.DB.prepare("DELETE FROM videos WHERE id=?1").bind(videoId).run();

    const after = await env.DB.prepare(
      `SELECT active_videos,active_mp4_videos,feed_videos,feed_mp4_videos,dirty
         FROM status_counts WHERE id=1`,
    ).first<Record<string, number>>();
    expect(after).toMatchObject({
      active_videos: 0,
      active_mp4_videos: 0,
      feed_videos: 0,
      feed_mp4_videos: 0,
      dirty: 0,
    });
    const rankings = await env.DB.prepare(
      "SELECT COUNT(*) AS count FROM ranking_entries WHERE video_id=?1",
    ).bind(videoId).first<{ count: number }>();
    expect(Number(rankings?.count)).toBe(0);
  });
});
