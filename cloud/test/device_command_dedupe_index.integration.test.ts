import { applyD1Migrations, env } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
});

describe("device command dedupe index", () => {
  it("uses the active-command index for exact payload lookup", async () => {
    const plan = await env.DB.prepare(
      `EXPLAIN QUERY PLAN
       SELECT id
         FROM device_commands
        WHERE device_id=?1
          AND command=?2
          AND payload IS ?3
          AND completed_at IS NULL
          AND (expires_at IS NULL OR expires_at>?4)
        ORDER BY id DESC
        LIMIT 1`,
    ).bind("homepanel-device", "restart_app", "{\"reason\":\"test\"}", Date.now())
      .all<{ detail: string }>();

    expect((plan.results ?? []).some(row => row.detail.includes("idx_device_commands_dedupe"))).toBe(true);
  });
});
