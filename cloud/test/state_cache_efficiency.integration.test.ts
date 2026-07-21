import { applyD1Migrations, env, SELF } from "cloudflare:test";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { invalidateStateCaches } from "../src/dashboard_cache";
import { readState, sha256Hex, updateState } from "../src/snapshot";
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

  it("writes the authoritative D1 version back when KV is stale", async () => {
    const startedAt = Date.now();
    await updateState(env, { source: "weather", observedAt: startedAt, payload: { temperature: 20 } });

    const externalPayload = JSON.stringify({ temperature: 21 });
    const externalHash = await sha256Hex(externalPayload);
    await env.DB.prepare(
      `UPDATE current_state SET version=3,payload=?1,observed_at=?2,fetched_at=?2,
         last_success_at=?2,status='ok',error=NULL,content_hash=?3
       WHERE source='weather'`,
    ).bind(externalPayload, startedAt + 1_000, externalHash).run();

    await updateState(env, {
      source: "weather",
      observedAt: startedAt + 2_000,
      payload: { temperature: 22 },
    });

    const current = await readState(env, "weather");
    expect(current?.version).toBe(4);
    expect(JSON.parse(current!.payload)).toEqual({ temperature: 22 });
  });
});
