import { applyD1Migrations, env } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
});

async function insertVideo(key: string): Promise<number> {
  const timestamp = "2026-07-23T00:00:00.000Z";
  const row = await env.DB.prepare(
    `INSERT INTO videos(media_url,canonical_key,media_type,first_seen_at,last_seen_at,status)
     VALUES(?1,?2,'mp4',?3,?3,'active')
     RETURNING id`,
  ).bind(`https://${key}`, key, timestamp).first<{ id: number }>();
  return Number(row?.id);
}

async function readStatus(id: number): Promise<string> {
  const row = await env.DB.prepare("SELECT status FROM videos WHERE id=?1")
    .bind(id).first<{ status: string }>();
  return String(row?.status ?? "");
}

describe("video liveness eligibility invariant", () => {
  it("keeps video status synchronized with block and death lists", async () => {
    const key = "media.test/invariant.mp4";
    const id = await insertVideo(key);
    const timestamp = "2026-07-23T00:00:00.000Z";

    await env.DB.prepare(
      `INSERT INTO video_blocklist(canonical_key,media_url,video_id,blocked_at,reason)
       VALUES(?1,?2,?3,?4,'test')`,
    ).bind(key, `https://${key}`, id, timestamp).run();
    expect(await readStatus(id)).toBe("hidden");

    await env.DB.prepare("DELETE FROM video_blocklist WHERE canonical_key=?1").bind(key).run();
    expect(await readStatus(id)).toBe("active");

    await env.DB.prepare(
      `INSERT INTO video_death_list(
         canonical_key,media_url,video_id,detected_at,last_checked_at,last_http_status,check_count
       ) VALUES(?1,?2,?3,?4,?4,404,1)`,
    ).bind(key, `https://${key}`, id, timestamp).run();
    expect(await readStatus(id)).toBe("dead");

    await env.DB.prepare(
      `INSERT INTO video_blocklist(canonical_key,media_url,video_id,blocked_at,reason)
       VALUES(?1,?2,?3,?4,'test')`,
    ).bind(key, `https://${key}`, id, timestamp).run();
    expect(await readStatus(id)).toBe("dead");

    await env.DB.prepare("DELETE FROM video_death_list WHERE canonical_key=?1").bind(key).run();
    expect(await readStatus(id)).toBe("hidden");

    await env.DB.prepare("DELETE FROM video_blocklist WHERE canonical_key=?1").bind(key).run();
    expect(await readStatus(id)).toBe("active");
  });

  it("keeps excluded rows out of the active liveness cursor", async () => {
    const timestamp = "2026-07-23T00:00:00.000Z";
    for (let index = 0; index < 8; index += 1) {
      const key = `media.test/blocked-${index}.mp4`;
      const id = await insertVideo(key);
      await env.DB.prepare(
        `INSERT INTO video_blocklist(canonical_key,media_url,video_id,blocked_at,reason)
         VALUES(?1,?2,?3,?4,'test')`,
      ).bind(key, `https://${key}`, id, timestamp).run();
    }
    const eligibleKey = "media.test/eligible.mp4";
    const eligibleId = await insertVideo(eligibleKey);

    const selected = await env.DB.prepare(
      `SELECT video.id, video.canonical_key AS canonicalKey
         FROM videos AS video
        WHERE video.id > ?1 AND video.id <= ?2
          AND video.status = 'active'
          AND NOT EXISTS (
            SELECT 1 FROM video_blocklist AS bad
             WHERE bad.canonical_key = video.canonical_key
          )
          AND NOT EXISTS (
            SELECT 1 FROM video_death_list AS death
             WHERE death.canonical_key = video.canonical_key
          )
        ORDER BY video.id
        LIMIT 1`,
    ).bind(0, eligibleId).first<{ id: number; canonicalKey: string }>();

    expect(selected).toEqual({ id: eligibleId, canonicalKey: eligibleKey });
    const activeExcluded = await env.DB.prepare(
      `SELECT COUNT(*) AS count
         FROM videos AS video
        WHERE video.status='active'
          AND (
            EXISTS (SELECT 1 FROM video_blocklist AS bad WHERE bad.canonical_key=video.canonical_key)
            OR EXISTS (SELECT 1 FROM video_death_list AS death WHERE death.canonical_key=video.canonical_key)
          )`,
    ).first<{ count: number }>();
    expect(Number(activeExcluded?.count)).toBe(0);
  });
});
