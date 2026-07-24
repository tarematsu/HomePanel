import { beforeEach, describe, expect, it } from "vitest";
import { applyD1Migrations, env, SELF } from "cloudflare:test";
import { acquireDueJobs } from "../src/scheduler";
import { ensureDashboard, readState, updateState } from "../src/snapshot";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

async function switchBotWebhookToken(): Promise<string> {
  const bytes = new TextEncoder().encode("test-secret:homepanel-webhook");
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(digest)].map(byte => byte.toString(16).padStart(2, "0")).join("").slice(0, 32);
}

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
});

describe("HomePanel Worker", () => {
  it("requires bearer auth and returns ETag/304 for dashboard JSON", async () => {
    await updateState(env, { source: "news", observedAt: Date.now(), payload: { headlines: [] } });
    expect((await SELF.fetch("https://example.test/v1/dashboard.json")).status).toBe(401);
    const first = await SELF.fetch("https://example.test/v1/dashboard.json", { headers: { Authorization: "Bearer test-device" } });
    expect(first.status).toBe(200);
    expect(first.headers.get("content-type")).toContain("application/json");
    const etag = first.headers.get("etag");
    expect(etag).toBeTruthy();
    const second = await SELF.fetch("https://example.test/v1/dashboard.json", {
      headers: { Authorization: "Bearer test-device", "If-None-Match": etag! },
    });
    expect(second.status).toBe(304);
  });

  it("preserves the last successful payload when a source becomes stale", async () => {
    await updateState(env, { source: "weather", observedAt: Date.now(), payload: { temperature: 20 } });
    const initialDashboard = await ensureDashboard(env);
    await updateState(env, { source: "weather", observedAt: Date.now(), payload: null }, "upstream failed");
    const state = await readState(env, "weather");
    const staleDashboard = await ensureDashboard(env);
    expect(state?.status).toBe("stale");
    expect(JSON.parse(state?.payload ?? "{}").temperature).toBe(20);
    expect(staleDashboard.status).toBe("stale");
    expect(staleDashboard.content_hash).not.toBe(initialDashboard.content_hash);
  });

  it("rebuilds the dashboard when a stale source recovers with unchanged content", async () => {
    const observedAt = Date.now();
    await updateState(env, { source: "weather", observedAt, payload: { temperature: 20 } });
    await updateState(env, { source: "weather", observedAt: observedAt + 1, payload: null }, "temporary failure");
    const staleDashboard = await ensureDashboard(env);

    await updateState(env, { source: "weather", observedAt: observedAt + 2, payload: { temperature: 20 } });
    const recovered = await readState(env, "weather");
    const recoveredDashboard = await ensureDashboard(env);

    expect(recovered?.status).toBe("ok");
    expect(staleDashboard.status).toBe("stale");
    expect(recoveredDashboard.status).toBe("ok");
    expect(recoveredDashboard.content_hash).not.toBe(staleDashboard.content_hash);
  });

  it("does not rebuild the dashboard when source content is unchanged", async () => {
    const firstObservedAt = Date.now();
    await updateState(env, { source: "weather", observedAt: firstObservedAt, payload: { temperature: 20, observedAt: firstObservedAt } });
    const initialDashboard = await ensureDashboard(env);

    await updateState(env, { source: "weather", observedAt: firstObservedAt + 60_000, payload: { temperature: 20, observedAt: firstObservedAt + 60_000 } });
    const nextDashboard = await ensureDashboard(env);

    expect(initialDashboard.version).toBeTruthy();
    expect(nextDashboard.version).toBe(initialDashboard.version);
    expect(nextDashboard.content_hash).toBe(initialDashboard.content_hash);
  });

  it("leases every due job once in batches of at most three", async () => {
    const now = Math.floor(Date.now() / 1000);
    await env.DB.prepare("UPDATE jobs SET next_run_at=?1, lease_until=NULL").bind(now - 1).run();
    const total = await env.DB.prepare("SELECT COUNT(*) AS count FROM jobs").first<{ count: number }>();
    const leasedNames: string[] = [];

    while (true) {
      const batch = await acquireDueJobs(env, now);
      expect(batch.length).toBeLessThanOrEqual(3);
      if (!batch.length) break;
      leasedNames.push(...batch.map(job => job.name));
    }

    expect(leasedNames).toHaveLength(Number(total?.count ?? 0));
    expect(new Set(leasedNames).size).toBe(leasedNames.length);
    expect(await acquireDueJobs(env, now)).toHaveLength(0);
  });

  it("leases only one due job for a CPU-bounded compatibility tick", async () => {
    const now = Math.floor(Date.now() / 1000);
    await env.DB.prepare("UPDATE jobs SET next_run_at=?1, lease_until=NULL").bind(now - 1).run();

    const first = await acquireDueJobs(env, now, 1);
    const second = await acquireDueJobs(env, now, 1);

    expect(first).toHaveLength(1);
    expect(second).toHaveLength(1);
    expect(second[0]?.name).not.toBe(first[0]?.name);
  });

  it("queues refresh work in the Scheduler DO without writing D1 jobs", async () => {
    const future = Math.floor(Date.now() / 1000) + 3600;
    await env.DB.prepare("UPDATE jobs SET next_run_at=?1, lease_until=NULL").bind(future).run();

    const response = await SELF.fetch("https://example.test/v1/refresh", {
      method: "POST",
      headers: {
        Authorization: "Bearer test-device",
        "Content-Type": "application/json",
      },
      body: JSON.stringify({ sources: ["weather"] }),
    });

    expect(response.status).toBe(202);
    await expect(response.json()).resolves.toEqual({ queued: true });
    const job = await env.DB.prepare(
      "SELECT next_run_at,lease_until FROM jobs WHERE name='weather'",
    ).first<{ next_run_at: number; lease_until: number | null }>();
    expect(job).toEqual({ next_run_at: future, lease_until: null });
    expect(await readState(env, "weather")).toBeNull();
  });

  it("stores SwitchBot webhook events and exposes the normalized state", async () => {
    const token = await switchBotWebhookToken();
    const response = await SELF.fetch(`https://example.test/v1/switchbot/webhook/${token}`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        eventType: "changeReport",
        eventVersion: "1",
        context: {
          deviceMac: "AABBCCDDEEFF",
          deviceType: "WoContact",
          timeOfSample: Date.now(),
          detectionState: "DETECTED",
          doorMode: "IN_DOOR",
          openState: "open",
          brightness: "bright",
          battery: 91,
        },
      }),
    });
    expect(response.status).toBe(204);

    const stored = await readState(env, "switchbot");
    const payload = JSON.parse(stored?.payload ?? "{}");
    expect(payload.presence).toBe("home");
    expect(payload.doorOpen).toBe(true);
    expect(payload.devices[0].deviceType).toBe("Contact Sensor");

    expect((await SELF.fetch("https://example.test/v1/switchbot")).status).toBe(401);
    const stateResponse = await SELF.fetch("https://example.test/v1/switchbot", { headers: { Authorization: "Bearer test-device" } });
    expect(stateResponse.status).toBe(200);
    expect(stateResponse.headers.get("etag")).toBeTruthy();
  });

  it("sets the SwitchBot fallback poll interval to fifteen minutes", async () => {
    const row = await env.DB.prepare("SELECT interval_seconds FROM jobs WHERE name='switchbot'").first<{ interval_seconds: number }>();
    expect(row?.interval_seconds).toBe(900);
  });
});
