import { applyD1Migrations, env, SELF } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
});

async function postTelemetry(body: unknown): Promise<Response> {
  return SELF.fetch("https://homepanel.test/v1/telemetry", {
    method: "POST",
    headers: {
      Authorization: "Bearer test-device",
      "content-type": "application/json",
    },
    body: JSON.stringify(body),
  });
}

describe("telemetry resilience", () => {
  it("applies a concurrently retried sample to its bucket only once", async () => {
    const observedAt = Date.now() - 1000;
    const body = {
      deviceId: "ci-device",
      samples: [{ sequence: 100, observedAt, co2: 725 }],
    };

    const responses = await Promise.all([postTelemetry(body), postTelemetry(body)]);
    const accepted = await Promise.all(responses.map(async response => {
      expect(response.status).toBe(200);
      return (await response.json<{ accepted: number }>()).accepted;
    }));
    expect(accepted.sort((left, right) => left - right)).toEqual([0, 1]);

    const bucket = await env.DB.prepare(
      "SELECT sample_count,co2_sum FROM environment_buckets WHERE device_id=?1",
    ).bind("ci-device").first<{ sample_count: number; co2_sum: number }>();
    expect(bucket).toMatchObject({ sample_count: 1, co2_sum: 725 });

    const sample = await env.DB.prepare(
      "SELECT bucket_applied FROM environment_samples WHERE device_id=?1 AND sequence=?2",
    ).bind("ci-device", 100).first<{ bucket_applied: number }>();
    expect(sample?.bucket_applied).toBe(1);
  });

  it("rebuilds the environment state when a committed sample is retried", async () => {
    const observedAt = Math.floor((Date.now() - 1000) / 300_000) * 300_000;
    const body = {
      deviceId: "ci-device",
      samples: [{ sequence: 200, observedAt, co2: 810 }],
    };

    expect((await postTelemetry(body)).status).toBe(200);
    await env.DB.prepare("DELETE FROM current_state WHERE source='environment'").run();

    const retry = await postTelemetry(body);
    await expect(retry.json()).resolves.toEqual({ accepted: 0 });

    const state = await env.DB.prepare(
      "SELECT payload FROM current_state WHERE source='environment'",
    ).first<{ payload: string }>();
    const payload = JSON.parse(state?.payload ?? "{}") as {
      history?: Array<{ t: number; co2: number | null }>;
    };
    expect(payload.history).toEqual([{ t: observedAt, co2: 810, temperature: null, humidity: null }]);
  });
});
