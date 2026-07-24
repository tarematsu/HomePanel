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

  it("suppresses unchanged current_state heartbeat writes for six hours", async () => {
    const startedAt = Date.now();
    vi.useFakeTimers();
    vi.setSystemTime(startedAt);
    const payload = { temperature: 20 };
    await updateState(env, { source: "weather", observedAt: startedAt, payload });
    const initial = await readState(env, "weather");

    vi.setSystemTime(startedAt + 5 * 60 * 60_000);
    await updateState(env, { source: "weather", observedAt: startedAt + 5 * 60 * 60_000, payload });
    const suppressed = await readState(env, "weather");
    expect(suppressed?.fetched_at).toBe(initial?.fetched_at);

    vi.setSystemTime(startedAt + 6 * 60 * 60_000 + 60_000);
    await updateState(env, { source: "weather", observedAt: startedAt + 6 * 60 * 60_000 + 60_000, payload });
    const written = await readState(env, "weather");
    expect(written?.fetched_at).toBe(startedAt + 6 * 60 * 60_000 + 60_000);
  });
});
