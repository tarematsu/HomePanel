import { applyD1Migrations, env, SELF } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
});

async function postSample(sequence: number, fields: Record<string, unknown>): Promise<Response> {
  return SELF.fetch("https://homepanel.test/v1/telemetry", {
    method: "POST",
    headers: {
      Authorization: "Bearer test-device",
      "content-type": "application/json",
    },
    body: JSON.stringify({
      deviceId: "ci-device",
      samples: [{ sequence, observedAt: Date.now() - 1000, ...fields }],
    }),
  });
}

describe("telemetry measurement validation", () => {
  it("rejects impossible or incorrectly typed measurements before writing to D1", async () => {
    const invalid = [
      { co2: 249 },
      { co2: 10_001 },
      { temperature: -40.01 },
      { temperature: 85.01 },
      { humidity: -0.01 },
      { humidity: 100.01 },
      { temperatureCorrected: -80.01 },
      { temperatureCorrected: 120.01 },
      { humidityCorrected: -0.01 },
      { humidityCorrected: 100.01 },
      { co2: "250" },
      { humidity: true },
      { co2: 500, sequence: "1012" },
      { co2: 500, observedAt: String(Date.now() - 1000) },
    ];

    for (let index = 0; index < invalid.length; index += 1) {
      const response = await postSample(1000 + index, invalid[index]!);
      expect(response.status).toBe(400);
      await expect(response.json()).resolves.toMatchObject({ error: "invalid telemetry sample" });
    }

    const stored = await env.DB.prepare(
      "SELECT COUNT(*) AS count FROM environment_samples",
    ).first<{ count: number }>();
    expect(Number(stored?.count ?? -1)).toBe(0);
  });

  it("accepts measurements exactly on every supported boundary", async () => {
    const response = await postSample(2000, {
      co2: 250,
      temperature: -40,
      humidity: 100,
      temperatureCorrected: 120,
      humidityCorrected: 0,
    });
    expect(response.status).toBe(200);

    const stored = await env.DB.prepare(
      `SELECT co2,temperature,humidity,temperature_corrected,humidity_corrected
         FROM environment_samples WHERE device_id=?1 AND sequence=?2`,
    ).bind("ci-device", 2000).first<Record<string, number>>();
    expect(stored).toMatchObject({
      co2: 250,
      temperature: -40,
      humidity: 100,
      temperature_corrected: 120,
      humidity_corrected: 0,
    });
  });
});
