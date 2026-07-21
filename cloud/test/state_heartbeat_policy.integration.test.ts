import { applyD1Migrations, env } from "cloudflare:test";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { readState, type StateRow, updateState } from "../src/snapshot";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

async function d1State(source: string): Promise<StateRow | null> {
  return env.DB.prepare(
    `SELECT source,version,payload,observed_at,fetched_at,last_success_at,status,error,content_hash
       FROM current_state WHERE source=?1`,
  ).bind(source).first<StateRow>();
}

beforeEach(async () => {
  vi.useFakeTimers();
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
  await env.DB.exec("DROP TRIGGER IF EXISTS skip_redundant_current_state_heartbeat");
});

afterEach(() => vi.useRealTimers());

describe("current_state checkpoint policy", () => {
  it("limits unchanged success KV heartbeats to thirty minutes and D1 to six hours", async () => {
    const startedAt = 1_800_000_000_000;
    vi.setSystemTime(startedAt);
    const payload = { temperature: 20 };
    await updateState(env, { source: "weather", observedAt: startedAt, payload });

    vi.setSystemTime(startedAt + 6 * 60_000);
    await updateState(env, { source: "weather", observedAt: startedAt + 6 * 60_000, payload });
    expect((await d1State("weather"))?.fetched_at).toBe(startedAt);
    expect((await readState(env, "weather"))?.fetched_at).toBe(startedAt);

    vi.setSystemTime(startedAt + 30 * 60_000);
    await updateState(env, { source: "weather", observedAt: startedAt + 30 * 60_000, payload });
    expect((await d1State("weather"))?.fetched_at).toBe(startedAt);
    expect((await readState(env, "weather"))?.fetched_at).toBe(startedAt + 30 * 60_000);

    vi.setSystemTime(startedAt + 6 * 60 * 60_000);
    await updateState(env, { source: "weather", observedAt: startedAt + 6 * 60 * 60_000, payload });
    expect((await d1State("weather"))?.fetched_at).toBe(startedAt + 6 * 60 * 60_000);
  });

  it("limits repeated error KV heartbeats to thirty minutes and D1 to six hours", async () => {
    const startedAt = 1_800_000_000_000;
    vi.setSystemTime(startedAt);
    await updateState(env, { source: "news", observedAt: startedAt, payload: null }, "upstream unavailable");

    vi.setSystemTime(startedAt + 6 * 60_000);
    await updateState(env, { source: "news", observedAt: startedAt + 6 * 60_000, payload: null }, "upstream unavailable");
    expect((await d1State("news"))?.fetched_at).toBe(startedAt);
    expect((await readState(env, "news"))?.fetched_at).toBe(startedAt);

    vi.setSystemTime(startedAt + 30 * 60_000);
    await updateState(env, {
      source: "news",
      observedAt: startedAt + 30 * 60_000,
      payload: null,
    }, "upstream unavailable");
    expect((await d1State("news"))?.fetched_at).toBe(startedAt);
    expect((await readState(env, "news"))?.fetched_at).toBe(startedAt + 30 * 60_000);

    vi.setSystemTime(startedAt + 6 * 60 * 60_000);
    await updateState(env, {
      source: "news",
      observedAt: startedAt + 6 * 60 * 60_000,
      payload: null,
    }, "upstream unavailable");
    expect((await d1State("news"))?.fetched_at).toBe(startedAt + 6 * 60 * 60_000);
  });
});
