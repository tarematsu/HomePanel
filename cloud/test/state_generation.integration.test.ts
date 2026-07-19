import { applyD1Migrations, env } from "cloudflare:test";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { invalidateStateCaches } from "../src/dashboard_cache";
import { updateState } from "../src/snapshot";
import { stateGeneration } from "../src/state_generation";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

beforeEach(async () => {
  vi.useFakeTimers();
  invalidateStateCaches(env);
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
});

afterEach(() => {
  vi.useRealTimers();
  invalidateStateCaches(env);
});

describe("state cache generations", () => {
  it("advances only when current_state is actually written", async () => {
    const startedAt = 1_800_000_000_000;
    const payload = { temperature: 20 };
    vi.setSystemTime(startedAt);

    await updateState(env, { source: "weather", observedAt: startedAt, payload });
    expect(stateGeneration(env)).toBe(1);

    vi.setSystemTime(startedAt + 6 * 60_000);
    await updateState(env, { source: "weather", observedAt: startedAt + 6 * 60_000, payload });
    expect(stateGeneration(env)).toBe(1);

    await updateState(env, {
      source: "weather",
      observedAt: startedAt + 6 * 60_000,
      payload: { temperature: 21 },
    });
    expect(stateGeneration(env)).toBe(2);
  });
});
