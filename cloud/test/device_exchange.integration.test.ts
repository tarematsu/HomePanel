import { applyD1Migrations, env, SELF } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { invalidateR2EnvironmentCache } from "../src/environment_r2";
import { stateGeneration } from "../src/state_generation";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & {
  DATA_BUCKET: R2Bucket;
  TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1];
};

function testEnv(): TestEnv {
  return env as TestEnv;
}

beforeEach(async () => {
  const bindings = testEnv();
  await resetD1TestDatabase(bindings.DB, bindings.TEST_MIGRATIONS);
  await bindings.DATA_BUCKET.delete("environment/v2/latest.json");
  invalidateR2EnvironmentCache(bindings);
});

function decodeExchange(bytes: Uint8Array): { payload: Record<string, unknown>; radar: Uint8Array } {
  expect(new TextDecoder().decode(bytes.slice(0, 8))).toBe("HPEX0001");
  const jsonLength = bytes[8]!
    | bytes[9]! << 8
    | bytes[10]! << 16
    | bytes[11]! << 24;
  expect(jsonLength).toBeGreaterThan(0);
  const jsonEnd = 12 + jsonLength;
  return {
    payload: JSON.parse(new TextDecoder().decode(bytes.slice(12, jsonEnd))) as Record<string, unknown>,
    radar: bytes.slice(jsonEnd),
  };
}

const versions = {
  dashboard: 0,
  radar: 0,
  switchbot: 0,
  stationhead: 0,
  stationheadHealth: 0,
  config: 0,
};

function exchange(body: unknown): Promise<Response> {
  return SELF.fetch("https://homepanel.test/v1/device/exchange?deviceId=homepanel-device", {
    method: "POST",
    headers: {
      Authorization: "Bearer test-device",
      "content-type": "application/json",
    },
    body: JSON.stringify(body),
  });
}

function telemetry(sequence: number, observedAt: number, co2 = 640): Record<string, unknown> {
  return {
    deviceId: "homepanel-device",
    appVersion: "2.12.0",
    stationheadOk: true,
    outboxCount: 1,
    samples: [{ sequence, observedAt, co2 }],
  };
}

describe("device exchange", () => {
  it("requires the configured device token", async () => {
    const response = await SELF.fetch("https://homepanel.test/v1/device/exchange?deviceId=homepanel-device", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ versions }),
    });
    expect(response.status).toBe(401);
  });

  it("returns sync and compact telemetry receipt in one binary response", async () => {
    const observedAt = Math.floor((Date.now() - 1000) / 900_000) * 900_000;
    const response = await exchange({ versions, telemetry: telemetry(1, observedAt) });
    expect(response.status).toBe(200);
    expect(response.headers.get("content-type")).toContain("application/vnd.homepanel.device-exchange");
    const decoded = decodeExchange(new Uint8Array(await response.arrayBuffer()));
    expect(decoded.radar).toHaveLength(0);
    expect(decoded.payload).toMatchObject({
      versions,
      telemetry: {
        accepted: 1,
        acknowledgedSequences: [1],
        nextSequence: 2,
      },
    });

    const legacySamples = await env.DB.prepare(
      "SELECT COUNT(*) AS count FROM environment_samples",
    ).first<{ count: number }>();
    const legacyBuckets = await env.DB.prepare(
      "SELECT COUNT(*) AS count FROM environment_buckets",
    ).first<{ count: number }>();
    expect(legacySamples?.count).toBe(0);
    expect(legacyBuckets?.count).toBe(0);

    const stored = await testEnv().DATA_BUCKET.get("environment/v2/latest.json");
    expect(stored).not.toBeNull();
    const document = await stored!.json<{
      lastSequences: Record<string, number>;
      row: { payload: string };
    }>();
    expect(document.lastSequences["homepanel-device"]).toBe(1);
    expect(JSON.parse(document.row.payload)).toMatchObject({
      deviceId: "homepanel-device",
      bucketMinutes: 15,
      history: [{ t: observedAt, co2: 640 }],
    });

    const refreshed = decodeExchange(new Uint8Array(await (await exchange({ versions })).arrayBuffer()));
    expect(refreshed.payload.versions).toMatchObject({ dashboard: 1 });
    expect(JSON.parse(String(refreshed.payload.dashboard))).toMatchObject({
      environment: {
        deviceId: "homepanel-device",
        bucketMinutes: 15,
        history: [{ t: observedAt, co2: 640 }],
      },
    });
  });

  it("acknowledges a retried telemetry batch without duplicating R2 aggregates", async () => {
    const observedAt = Math.floor((Date.now() - 1000) / 900_000) * 900_000;
    const request = () => exchange({ versions, telemetry: telemetry(1, observedAt) });

    expect((await request()).status).toBe(200);
    const retried = decodeExchange(new Uint8Array(await (await request()).arrayBuffer()));
    expect(retried.payload.telemetry).toMatchObject({
      accepted: 0,
      acknowledgedSequences: [1],
      nextSequence: 2,
    });

    const stored = await testEnv().DATA_BUCKET.get("environment/v2/latest.json");
    const document = await stored!.json<{ row: { payload: string } }>();
    const payload = JSON.parse(document.row.payload) as {
      history: Array<{ t: number; co2: number }>;
    };
    expect(payload.history).toEqual([{ t: observedAt, co2: 640, temperature: null, humidity: null }]);
  });

  it("does not move environment observed_at backwards for delayed samples", async () => {
    const recent = Math.floor((Date.now() - 60_000) / 900_000) * 900_000;
    const delayed = recent - 900_000;
    expect((await exchange({ versions, telemetry: telemetry(1, recent) })).status).toBe(200);
    expect((await exchange({ versions, telemetry: telemetry(2, delayed, 620) })).status).toBe(200);

    const stored = await testEnv().DATA_BUCKET.get("environment/v2/latest.json");
    const document = await stored!.json<{ row: { observed_at: number } }>();
    expect(document.row.observed_at).toBe(recent);
  });

  it("does not invalidate dashboard state when an accepted sample leaves content unchanged", async () => {
    const observedAt = Math.floor((Date.now() - 1000) / 900_000) * 900_000;
    expect((await exchange({ versions, telemetry: telemetry(1, observedAt) })).status).toBe(200);
    const generation = stateGeneration(env);

    expect((await exchange({ versions, telemetry: telemetry(2, observedAt) })).status).toBe(200);
    expect(stateGeneration(env)).toBe(generation);
  });
});
