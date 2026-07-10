import { applyD1Migrations, env, SELF } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };
type TelemetryReceipt = {
  accepted: number;
  acknowledgedSequences: number[];
  nextSequence: number;
  expired?: number;
};

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

async function telemetryReceipt(body: unknown): Promise<TelemetryReceipt> {
  const response = await postTelemetry(body);
  expect(response.status).toBe(200);
  return response.json<TelemetryReceipt>();
}

describe("telemetry resilience", () => {
  it("applies a concurrently retried sample to its bucket only once", async () => {
    const observedAt = Date.now() - 1000;
    const body = {
      deviceId: "ci-device",
      samples: [{ sequence: 100, observedAt, co2: 725 }],
    };

    const receipts = await Promise.all([telemetryReceipt(body), telemetryReceipt(body)]);
    expect(receipts.map(receipt => receipt.acknowledgedSequences)).toEqual([[100], [100]]);
    expect(receipts.map(receipt => receipt.nextSequence)).toEqual([101, 101]);

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

    expect((await telemetryReceipt(body)).acknowledgedSequences).toEqual([200]);
    await env.DB.prepare("DELETE FROM current_state WHERE source='environment'").run();

    const retry = await telemetryReceipt(body);
    expect(retry.acknowledgedSequences).toEqual([200]);

    const state = await env.DB.prepare(
      "SELECT payload FROM current_state WHERE source='environment'",
    ).first<{ payload: string }>();
    const payload = JSON.parse(state?.payload ?? "{}") as {
      history?: Array<{ t: number; co2: number | null }>;
    };
    expect(payload.history).toEqual([{ t: observedAt, co2: 810, temperature: null, humidity: null }]);
  });

  it("accepts a reused sequence when the replacement sample is newer", async () => {
    const firstAt = Math.floor((Date.now() - 20 * 60_000) / 300_000) * 300_000;
    const secondAt = firstAt + 300_000;

    expect((await telemetryReceipt({
      deviceId: "ci-device",
      samples: [{ sequence: 300, observedAt: firstAt, co2: 700 }],
    })).acknowledgedSequences).toEqual([300]);
    expect((await telemetryReceipt({
      deviceId: "ci-device",
      samples: [{ sequence: 300, observedAt: secondAt, co2: 900 }],
    })).acknowledgedSequences).toEqual([300]);

    const buckets = await env.DB.prepare(
      "SELECT bucket_at,sample_count,co2_sum FROM environment_buckets WHERE device_id=?1 ORDER BY bucket_at",
    ).bind("ci-device").all<{ bucket_at: number; sample_count: number; co2_sum: number }>();
    expect(buckets.results).toEqual([
      { bucket_at: firstAt, sample_count: 1, co2_sum: 700 },
      { bucket_at: secondAt, sample_count: 1, co2_sum: 900 },
    ]);
  });

  it("returns a rebase floor instead of acknowledging an older sequence collision", async () => {
    const newerAt = Date.now() - 1000;
    await telemetryReceipt({
      deviceId: "ci-device",
      samples: [{ sequence: 400, observedAt: newerAt, co2: 700 }],
    });

    const collision = await telemetryReceipt({
      deviceId: "ci-device",
      samples: [{ sequence: 400, observedAt: newerAt - 5000, co2: 800 }],
    });
    expect(collision.acknowledgedSequences).toEqual([]);
    expect(collision.nextSequence).toBe(401);
  });

  it("acknowledges expired samples without retaining them", async () => {
    const oldAt = Date.now() - 31 * 24 * 60 * 60 * 1000;
    const receipt = await telemetryReceipt({
      deviceId: "ci-device",
      samples: [{ sequence: 500, observedAt: oldAt, co2: 650 }],
    });
    expect(receipt.acknowledgedSequences).toEqual([500]);
    expect(receipt.expired).toBe(1);

    const stored = await env.DB.prepare(
      "SELECT COUNT(*) AS count FROM environment_samples WHERE device_id=?1 AND sequence=?2",
    ).bind("ci-device", 500).first<{ count: number }>();
    expect(stored?.count).toBe(0);
  });
});
