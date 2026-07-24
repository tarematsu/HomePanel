import { applyD1Migrations, env } from "cloudflare:test";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { readState, updateState } from "../src/snapshot";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

beforeEach(async () => {
  vi.useFakeTimers();
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
  await env.DB.exec("DROP TRIGGER IF EXISTS skip_redundant_current_state_heartbeat");
});

afterEach(() => vi.useRealTimers());

describe("current_state heartbeat policy", () => {
  it("does not issue an unchanged success heartbeat before six hours", async () => {
    const startedAt = 1_800_000_000_000;
    vi.setSystemTime(startedAt);
    const payload = { temperature: 20 };
    await updateState(env, { source: "weather", observedAt: startedAt, payload });

    vi.setSystemTime(startedAt + 5 * 60 * 60_000);
    await updateState(env, { source: "weather", observedAt: startedAt + 5 * 60 * 60_000, payload });
    expect((await readState(env, "weather"))?.fetched_at).toBe(startedAt);

    vi.setSystemTime(startedAt + 6 * 60 * 60_000);
    await updateState(env, { source: "weather", observedAt: startedAt + 6 * 60 * 60_000, payload });
    expect((await readState(env, "weather"))?.fetched_at).toBe(startedAt + 6 * 60 * 60_000);
  });

  it("does not rewrite an identical error heartbeat before six hours", async () => {
    const startedAt = 1_800_000_000_000;
    vi.setSystemTime(startedAt);
    await updateState(env, { source: "news", observedAt: startedAt, payload: null }, "upstream unavailable");

    vi.setSystemTime(startedAt + 5 * 60 * 60_000);
    await updateState(env, { source: "news", observedAt: startedAt + 5 * 60 * 60_000, payload: null }, "upstream unavailable");
    expect((await readState(env, "news"))?.fetched_at).toBe(startedAt);

    vi.setSystemTime(startedAt + 6 * 60 * 60_000);
    await updateState(env, { source: "news", observedAt: startedAt + 6 * 60 * 60_000, payload: null }, "upstream unavailable");
    expect((await readState(env, "news"))?.fetched_at).toBe(startedAt + 6 * 60 * 60_000);
  });
});
