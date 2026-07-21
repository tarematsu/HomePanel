import { applyD1Migrations, env } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { requestRefresh } from "../src/scheduler";
import { finishSelectedJob, selectDueJob } from "../src/scheduler_tick";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
  const now = Math.floor(Date.now() / 1000);
  await env.DB.prepare("UPDATE jobs SET next_run_at=?1,lease_until=NULL,last_success_at=?2")
    .bind(now + 3600, now)
    .run();
});

describe("single-write scheduler tick", () => {
  it("selects a due job without taking a D1 lease and advances it once", async () => {
    const now = Math.floor(Date.now() / 1000);
    await env.DB.prepare("UPDATE jobs SET next_run_at=?1 WHERE name='weather'").bind(now - 1).run();

    const selected = await selectDueJob(env, now);
    expect(selected?.name).toBe("weather");
    expect(selected?.next_run_at).toBe(now - 1);
    expect(selected?.lease_until).toBeNull();

    const before = await env.DB.prepare(
      "SELECT next_run_at,lease_until FROM jobs WHERE name='weather'",
    ).first<{ next_run_at: number; lease_until: number | null }>();
    expect(before).toEqual({ next_run_at: now - 1, lease_until: null });

    expect(await finishSelectedJob(env, selected!, { startedAt: now, success: true }, now)).toBe(true);
    const after = await env.DB.prepare(
      "SELECT next_run_at,lease_until,last_success_at FROM jobs WHERE name='weather'",
    ).first<{ next_run_at: number; lease_until: number | null; last_success_at: number | null }>();
    expect(after).toEqual({
      next_run_at: now + Number(selected?.interval_seconds),
      lease_until: null,
      last_success_at: now,
    });
  });

  it("preserves a refresh queued while the selected job is running", async () => {
    const now = Math.floor(Date.now() / 1000);
    await env.DB.prepare("UPDATE jobs SET next_run_at=?1 WHERE name='weather'").bind(now - 1).run();
    const selected = await selectDueJob(env, now);
    expect(selected?.name).toBe("weather");

    expect(await requestRefresh(env, ["weather"])).toBe(true);
    expect(await finishSelectedJob(env, selected!, { startedAt: now, success: true }, now)).toBe(false);

    const row = await env.DB.prepare(
      "SELECT next_run_at,lease_until FROM jobs WHERE name='weather'",
    ).first<{ next_run_at: number; lease_until: number | null }>();
    expect(row).toEqual({ next_run_at: 0, lease_until: null });
  });
});
