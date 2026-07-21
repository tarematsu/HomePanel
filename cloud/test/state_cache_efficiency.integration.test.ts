import { applyD1Migrations, env, SELF } from "cloudflare:test";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { invalidateStateCaches } from "../src/dashboard_cache";
import { readState, updateState } from "../src/snapshot";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

const headers = { Authorization: "Bearer test-device" };

beforeEach(async () => {
  vi.useRealTimers();
  invalidateStateCaches(env);
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
});

afterEach(() => {
  vi.useRealTimers();
  invalidateStateCaches(env);
});

describe("state cache and D1 efficiency", () => {
  it("does not return an expired dashboard ETag after state changes", async () => {
    const startedAt = Date.now();
    vi.useFakeTimers();
    vi.setSystemTime(startedAt);
    await updateState(env, { source: "news", observedAt: startedAt, payload: { headlines: ["old"] } });

    const first = await SELF.fetch("https://homepanel.test/v1/dashboard.json", { headers });
    const oldEtag = first.headers.get("etag");
    expect(first.status).toBe(200);
    expect(oldEtag).toBeTruthy();

    vi.setSystemTime(startedAt + 61_000);
    await updateState(env, {
      source: "news",
      observedAt: startedAt + 61_000,
      payload: { headlines: ["new"] },
    });
    const refreshed = await SELF.fetch("https://homepanel.test/v1/dashboard.json", {
      headers: { ...headers, "If-None-Match": oldEtag! },
    });

    expect(refreshed.status).toBe(200);
    expect(refreshed.headers.get("etag")).not.toBe(oldEtag);
  });

  it("serves repeated meta validation from the in-isolate cache", async () => {
    await updateState(env, { source: "weather", observedAt: Date.now(), payload: { temperature: 20 } });
    const first = await SELF.fetch("https://homepanel.test/v1/meta", { headers });
    const etag = first.headers.get("etag");
    expect(first.status).toBe(200);
    expect(etag).toBeTruthy();

    const second = await SELF.fetch("https://homepanel.test/v1/meta", {
      headers: { ...headers, "If-None-Match": etag! },
    });
    expect(second.status).toBe(304);
  });

  it("serves a valid state row from KV when the D1 row is unavailable", async () => {
    const observedAt = Date.now();
    await updateState(env, { source: "weather", observedAt, payload: { temperature: 20 } });
    await env.DB.prepare("DELETE FROM current_state WHERE source='weather'").run();

    const cached = await readState(env, "weather");
    expect(cached).toMatchObject({
      source: "weather",
      observed_at: observedAt,
      status: "ok",
    });
    expect(JSON.parse(cached!.payload)).toEqual({ temperature: 20 });
  });
});
