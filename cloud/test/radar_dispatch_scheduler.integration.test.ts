import { applyD1Migrations, env } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { ensureSystemJobs, invalidateSystemJobsCache } from "../src/scheduler";
import type { Env } from "../src/sources";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

beforeEach(async () => {
  const testEnv = env as TestEnv;
  invalidateSystemJobsCache(testEnv.DB);
  await applyD1Migrations(testEnv.DB, testEnv.TEST_MIGRATIONS);
  await env.DB.prepare("DELETE FROM jobs WHERE name='radar_dispatch'").run();
});

describe("radar dispatch scheduler registration", () => {
  it("does not register the dispatcher without the GitHub token", async () => {
    await ensureSystemJobs(env as unknown as Env);

    const row = await env.DB.prepare(
      "SELECT name FROM jobs WHERE name='radar_dispatch'",
    ).first<{ name: string }>();
    expect(row).toBeNull();
  });

  it("registers a five-minute dispatcher when the GitHub token is configured", async () => {
    await ensureSystemJobs({
      ...(env as unknown as Env),
      GITHUB_RADAR_DISPATCH_TOKEN: "configured-token",
    });

    const row = await env.DB.prepare(
      "SELECT interval_seconds,next_run_at,lease_until,consecutive_failures FROM jobs WHERE name='radar_dispatch'",
    ).first<{
      interval_seconds: number;
      next_run_at: number;
      lease_until: number | null;
      consecutive_failures: number;
    }>();
    expect(row).toEqual({
      interval_seconds: 300,
      next_run_at: 0,
      lease_until: null,
      consecutive_failures: 0,
    });
  });

  it("removes the dispatcher when the GitHub token is withdrawn", async () => {
    const configuredEnv = {
      ...(env as unknown as Env),
      GITHUB_RADAR_DISPATCH_TOKEN: "configured-token",
    };
    await ensureSystemJobs(configuredEnv);
    expect(await env.DB.prepare(
      "SELECT name FROM jobs WHERE name='radar_dispatch'",
    ).first<{ name: string }>()).toEqual({ name: "radar_dispatch" });

    await ensureSystemJobs(env as unknown as Env);

    expect(await env.DB.prepare(
      "SELECT name FROM jobs WHERE name='radar_dispatch'",
    ).first<{ name: string }>()).toBeNull();
  });
});
