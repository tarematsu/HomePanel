import { applyD1Migrations, env } from "cloudflare:test";
import { beforeEach, describe, expect, it, vi } from "vitest";
import { ensureSystemJobs } from "../src/scheduler";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
  vi.useRealTimers();
});

describe("Octopus schedule", () => {
  it("normalizes the Octopus job to a 24-hour interval", async () => {
    vi.useFakeTimers();
    vi.setSystemTime(new Date("2026-07-11T03:00:00Z"));
    const nowSeconds = Math.floor(Date.now() / 1000);

    await env.DB.prepare(
      `INSERT INTO jobs(name,interval_seconds,next_run_at,lease_until,last_success_at,last_error,consecutive_failures)
       VALUES('octopus',3600,?1,NULL,NULL,NULL,0)
       ON CONFLICT(name) DO UPDATE SET interval_seconds=3600,next_run_at=?1`,
    ).bind(nowSeconds + 3600).run();

    await ensureSystemJobs(env);

    const job = await env.DB.prepare(
      "SELECT interval_seconds,next_run_at FROM jobs WHERE name='octopus'",
    ).first<{ interval_seconds: number; next_run_at: number }>();
    expect(job?.interval_seconds).toBe(86400);
    expect(job?.next_run_at).toBe(nowSeconds + 3600);
  });

  it("preserves an already queued immediate refresh", async () => {
    await env.DB.prepare(
      "UPDATE jobs SET interval_seconds=3600,next_run_at=0 WHERE name='octopus'",
    ).run();

    await ensureSystemJobs(env);

    const job = await env.DB.prepare(
      "SELECT interval_seconds,next_run_at FROM jobs WHERE name='octopus'",
    ).first<{ interval_seconds: number; next_run_at: number }>();
    expect(job).toEqual({ interval_seconds: 86400, next_run_at: 0 });
  });
});
