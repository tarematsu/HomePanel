import { applyD1Migrations, env, SELF } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };
type TelemetryReceipt = {
  accepted: number;
  acknowledgedSequences: number[];
  nextSequence: number;
  duplicates?: number;
};

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
});

const auth = (token = "test-device"): HeadersInit => ({ Authorization: `Bearer ${token}` });

async function postTelemetry(body: unknown, token = "test-device"): Promise<Response> {
  return SELF.fetch("https://homepanel.test/v1/telemetry", {
    method: "POST",
    headers: { ...auth(token), "content-type": "application/json" },
    body: JSON.stringify(body),
  });
}

async function readTelemetry(response: Response): Promise<TelemetryReceipt> {
  expect(response.status).toBe(200);
  return response.json<TelemetryReceipt>();
}

describe("HomePanel Worker integration", () => {
  it("serves health and protects device reads", async () => {
    const health = await SELF.fetch("https://homepanel.test/v1/health");
    expect(health.status).toBe(200);
    expect((await health.json<{ ok: boolean }>()).ok).toBe(true);
    expect((await SELF.fetch("https://homepanel.test/v1/meta")).status).toBe(401);
    expect((await SELF.fetch("https://homepanel.test/v1/update/file/HomePanel.exe")).status).toBe(401);
  });

  it("rejects malformed telemetry without writing a bucket", async () => {
    const response = await postTelemetry({
      deviceId: "bad/device",
      samples: [{ sequence: 1, observedAt: Date.now(), co2: 500 }],
    });
    expect(response.status).toBe(400);
    const row = await env.DB.prepare("SELECT COUNT(*) AS count FROM environment_buckets").first<{ count: number }>();
    expect(Number(row?.count)).toBe(0);
  });

  it("stores compact buckets, ignores diagnostics, and deduplicates retries", async () => {
    const body = {
      deviceId: "ci-device",
      appVersion: "2.0.123",
      stationheadOk: true,
      outboxCount: 3,
      diagnostics: { ignored: true },
      samples: [{
        sequence: 10,
        observedAt: Date.now() - 1000,
        co2: 650,
        temperature: 24.25,
        humidity: 48.5,
        temperatureCorrected: 24.1,
        humidityCorrected: 48.2,
      }],
    };
    const first = await readTelemetry(await postTelemetry(body));
    const second = await readTelemetry(await postTelemetry(body));
    expect(first).toMatchObject({ accepted: 1, acknowledgedSequences: [10], nextSequence: 11 });
    expect(second).toMatchObject({ accepted: 0, acknowledgedSequences: [10], nextSequence: 11 });

    const bucket = await env.DB.prepare(
      "SELECT sample_count,co2_sum,temperature_sum,humidity_sum FROM environment_buckets WHERE device_id=?1",
    ).bind("ci-device").first<Record<string, unknown>>();
    expect(bucket).toMatchObject({ sample_count: 1, co2_sum: 650, temperature_sum: 24.1, humidity_sum: 48.2 });

    const heartbeat = await env.DB.prepare(
      "SELECT payload,last_sequence FROM device_heartbeats WHERE device_id=?1",
    ).bind("ci-device").first<Record<string, unknown>>();
    expect(heartbeat).toMatchObject({ payload: null, last_sequence: 10 });
    const metrics = await env.DB.prepare("SELECT COUNT(*) AS count FROM device_metrics").first<{ count: number }>();
    expect(Number(metrics?.count)).toBe(0);
  });

  it("deduplicates equal sequences inside one telemetry request", async () => {
    const observedAt = Date.now() - 1000;
    const receipt = await readTelemetry(await postTelemetry({
      deviceId: "ci-device",
      samples: [
        { sequence: 20, observedAt, co2: 700 },
        { sequence: 20, observedAt, co2: 999 },
      ],
    }));
    expect(receipt).toMatchObject({
      accepted: 1,
      acknowledgedSequences: [20],
      nextSequence: 21,
      duplicates: 1,
    });
    const bucket = await env.DB.prepare(
      "SELECT sample_count,co2_sum FROM environment_buckets WHERE device_id=?1",
    ).bind("ci-device").first<{ sample_count: number; co2_sum: number }>();
    expect(bucket).toMatchObject({ sample_count: 1, co2_sum: 700 });
  });

  it("accepts a recovered outbox batch larger than 500 samples", async () => {
    const observedAt = Date.now() - 1000;
    const samples = Array.from({ length: 501 }, (_, index) => ({
      sequence: index + 1,
      observedAt,
      co2: 500 + index % 10,
    }));
    const receipt = await readTelemetry(await postTelemetry({ deviceId: "ci-device", samples }));
    expect(receipt.accepted).toBe(501);
    expect(receipt.acknowledgedSequences).toHaveLength(501);
    expect(receipt.acknowledgedSequences[0]).toBe(1);
    expect(receipt.acknowledgedSequences.at(-1)).toBe(501);
    expect(receipt.nextSequence).toBe(502);
    const bucket = await env.DB.prepare(
      "SELECT sample_count FROM environment_buckets WHERE device_id=?1",
    ).bind("ci-device").first<{ sample_count: number }>();
    expect(bucket?.sample_count).toBe(501);
  });

  it("keeps multi-batch telemetry retry-safe across distinct buckets", async () => {
    const latestBucket = Math.floor((Date.now() - 60_000) / 300_000) * 300_000;
    const samples = Array.from({ length: 120 }, (_, index) => ({
      sequence: index + 1,
      observedAt: latestBucket - index * 300_000,
      co2: 600 + index,
    }));
    const body = { deviceId: "ci-device", appVersion: "2.7.0", samples };
    const first = await readTelemetry(await postTelemetry(body));
    const retry = await readTelemetry(await postTelemetry(body));
    expect(first.accepted).toBe(120);
    expect(first.acknowledgedSequences).toHaveLength(120);
    expect(first.nextSequence).toBe(121);
    expect(retry.accepted).toBe(0);
    expect(retry.acknowledgedSequences).toHaveLength(120);
    expect(retry.nextSequence).toBe(121);

    const aggregate = await env.DB.prepare(
      "SELECT COUNT(*) AS buckets,SUM(sample_count) AS samples FROM environment_buckets WHERE device_id=?1",
    ).bind("ci-device").first<{ buckets: number; samples: number }>();
    expect(aggregate).toMatchObject({ buckets: 120, samples: 120 });
    const heartbeat = await env.DB.prepare(
      "SELECT last_sequence FROM device_heartbeats WHERE device_id=?1",
    ).bind("ci-device").first<{ last_sequence: number }>();
    expect(heartbeat?.last_sequence).toBe(120);
  });

  it("isolates device commands by configured device token", async () => {
    const created = await SELF.fetch("https://homepanel.test/v1/device/commands", {
      method: "POST",
      headers: { ...auth("test-action"), "content-type": "application/json" },
      body: JSON.stringify({ deviceId: "device-b", command: "check_update" }),
    });
    expect(created.status).toBe(202);
    expect((await SELF.fetch("https://homepanel.test/v1/device/commands?deviceId=device-b", { headers: auth("token-a") })).status).toBe(401);
    const allowed = await SELF.fetch("https://homepanel.test/v1/device/commands?deviceId=device-b", { headers: auth("token-b") });
    expect(allowed.status).toBe(200);
    expect((await allowed.json<{ commands: unknown[] }>()).commands).toHaveLength(1);
  });

  it("uses optimistic locking and creates one pending restart command", async () => {
    const url = "https://homepanel.test/v1/device/config?deviceId=homepanel-device";
    const withoutPrecondition = await SELF.fetch(url, {
      method: "PUT",
      headers: { ...auth("test-action"), "content-type": "application/json" },
      body: JSON.stringify({ cloudPollSeconds: 60 }),
    });
    expect(withoutPrecondition.status).toBe(428);

    const saved = await SELF.fetch(url, {
      method: "PUT",
      headers: { ...auth("test-action"), "content-type": "application/json", "If-Match": '"device-config-homepanel-device-0"' },
      body: JSON.stringify({ cloudPollSeconds: 60 }),
    });
    expect(saved.status).toBe(200);
    expect(saved.headers.get("etag")).toBe('"device-config-homepanel-device-1"');

    const stale = await SELF.fetch(url, {
      method: "PUT",
      headers: { ...auth("test-action"), "content-type": "application/json", "If-Match": '"device-config-homepanel-device-0"' },
      body: JSON.stringify({ cloudPollSeconds: 30 }),
    });
    expect(stale.status).toBe(412);
    const pending = await env.DB.prepare(
      "SELECT COUNT(*) AS count FROM device_commands WHERE device_id=?1 AND command='restart_app' AND completed_at IS NULL",
    ).bind("homepanel-device").first<{ count: number }>();
    expect(Number(pending?.count)).toBe(1);
  });

  it("updates one device history incrementally while preserving other devices", async () => {
    const now = Math.floor((Date.now() - 1000) / 300_000) * 300_000;
    expect((await postTelemetry({ deviceId: "device-a", samples: [{ sequence: 1, observedAt: now, co2: 600 }] }, "token-a")).status).toBe(200);
    expect((await postTelemetry({ deviceId: "device-b", samples: [{ sequence: 1, observedAt: now, co2: 800 }] }, "token-b")).status).toBe(200);
    expect((await postTelemetry({ deviceId: "device-a", samples: [{ sequence: 2, observedAt: now, co2: 1000 }] }, "token-a")).status).toBe(200);

    const response = await SELF.fetch("https://homepanel.test/v1/dashboard.json", { headers: auth("token-a") });
    const dashboard = await response.json<{
      environment: {
        deviceId: string;
        devices: Record<string, { history: Array<{ t: number; co2: number | null }> }>;
      };
    }>();
    expect(dashboard.environment.deviceId).toBe("device-a");
    expect(Object.keys(dashboard.environment.devices).sort()).toEqual(["device-a", "device-b"]);
    expect(dashboard.environment.devices["device-a"]?.history).toEqual([{ t: now, co2: 800, temperature: null, humidity: null }]);
    expect(dashboard.environment.devices["device-b"]?.history).toEqual([{ t: now, co2: 800, temperature: null, humidity: null }]);
  });

  it("rejects radar tile coordinates outside the zoom range", async () => {
    const response = await SELF.fetch("https://homepanel.test/v1/radar/tile/gsi/1/999999999/999999999.png");
    expect(response.status).toBe(404);
  });

  it("serves an admin default that uses the Worker update manifest", async () => {
    const response = await SELF.fetch("https://homepanel.test/admin");
    const page = await response.text();
    expect(page).toContain('/v1/update/manifest');
    expect(page).not.toContain("releases/download/homepanel-latest/manifest.json");
  });

  it("returns a stable JSON 404", async () => {
    const response = await SELF.fetch("https://homepanel.test/missing");
    expect(response.status).toBe(404);
    await expect(response.json()).resolves.toEqual({ error: "not found" });
  });
});
