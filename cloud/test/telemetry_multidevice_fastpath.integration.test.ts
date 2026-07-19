import { applyD1Migrations, env, SELF } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
});

function postTelemetry(deviceId: string, token: string, sequence: number, observedAt: number, co2: number): Promise<Response> {
  return SELF.fetch("https://homepanel.test/v1/telemetry", {
    method: "POST",
    headers: { Authorization: `Bearer ${token}`, "content-type": "application/json" },
    body: JSON.stringify({ deviceId, samples: [{ sequence, observedAt, co2 }] }),
  });
}

describe("multi-device telemetry state fast path", () => {
  it("preserves another device without reading its durable buckets again", async () => {
    const bucketAt = Math.floor((Date.now() - 1000) / 300_000) * 300_000;
    expect((await postTelemetry("device-a", "token-a", 1, bucketAt, 600)).status).toBe(200);
    expect((await postTelemetry("device-b", "token-b", 1, bucketAt, 800)).status).toBe(200);

    await env.DB.prepare("DELETE FROM environment_buckets WHERE device_id='device-b'").run();
    expect((await postTelemetry("device-a", "token-a", 2, bucketAt, 1000)).status).toBe(200);

    const response = await SELF.fetch("https://homepanel.test/v1/dashboard.json", {
      headers: { Authorization: "Bearer token-a" },
    });
    expect(response.status).toBe(200);
    const dashboard = await response.json<{
      environment: {
        devices: Record<string, { history: Array<{ t: number; co2: number | null }> }>;
      };
    }>();
    expect(Object.keys(dashboard.environment.devices).sort()).toEqual(["device-a", "device-b"]);
    expect(dashboard.environment.devices["device-a"]?.history).toEqual([
      { t: bucketAt, co2: 800, temperature: null, humidity: null },
    ]);
    expect(dashboard.environment.devices["device-b"]?.history).toEqual([
      { t: bucketAt, co2: 800, temperature: null, humidity: null },
    ]);
  });
});
