import { applyD1Migrations, env } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { acquireDueJobs, finishJob } from "../src/scheduler";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
  const now = Math.floor(Date.now() / 1000);
  await env.DB.prepare(
    `UPDATE jobs SET next_run_at=?1,lease_until=NULL,last_success_at=?2,
       last_error=NULL,consecutive_failures=0`,
  ).bind(now + 3600, now - 60).run();
});

describe("scheduler success checkpoints", () => {
  it("keeps a recent successful lease as the completion checkpoint", async () => {
    const now = Math.floor(Date.now() / 1000);
    await env.DB.prepare(
      "UPDATE jobs SET next_run_at=?1 WHERE name='weather'",
    ).bind(now - 1).run();

    const weather = (await acquireDueJobs(env, now)).find(job => job.name === "weather");
    expect(weather).toBeTruthy();
    expect(Number(weather?.next_run_at)).toBeGreaterThan(now);
    expect(await finishJob(env, weather!, now, true, undefined, now + 1)).toBe(true);

    const row = await env.DB.prepare(
      "SELECT next_run_at,lease_until,last_success_at FROM jobs WHERE name='weather'",
    ).first<{ next_run_at: number; lease_until: number | null; last_success_at: number | null }>();
    expect(row).toEqual({
      next_run_at: weather?.next_run_at,
      lease_until: weather?.lease_until,
      last_success_at: now - 60,
    });
  });

  it("writes a forced run completion immediately", async () => {
    const now = Math.floor(Date.now() / 1000);
    await env.DB.prepare(
      "UPDATE jobs SET next_run_at=0 WHERE name='weather'",
    ).run();

    const weather = (await acquireDueJobs(env, now)).find(job => job.name === "weather");
    expect(Number(weather?.next_run_at)).toBeLessThan(0);
    expect(await finishJob(env, weather!, now, true, undefined, now + 1)).toBe(true);

    const row = await env.DB.prepare(
      "SELECT next_run_at,lease_until,last_success_at FROM jobs WHERE name='weather'",
    ).first<{ next_run_at: number; lease_until: number | null; last_success_at: number | null }>();
    expect(Number(row?.next_run_at)).toBeGreaterThan(now);
    expect(row?.lease_until).toBeNull();
    expect(row?.last_success_at).toBe(now + 1);
  });
});
