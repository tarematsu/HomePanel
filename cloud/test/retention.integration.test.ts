import { applyD1Migrations, env } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { cleanupExpiredData } from "../src/scheduler";
import { resetD1TestDatabase } from "./d1_test_utils";

 type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
});

describe("telemetry retention", () => {
  it("retains environment and Octopus daily history beyond cleanup limits", async () => {
    const now = Date.now();
    const day = 86_400_000;
    const insertSample = (sequence: number, observedAt: number, applied: number) => env.DB.prepare(
      `INSERT INTO environment_samples(
         device_id,sequence,observed_at,co2,temperature,humidity,
         temperature_corrected,humidity_corrected,bucket_applied
       ) VALUES('ci-device',?1,?2,700,24.5,50.0,24.0,49.0,?3)`,
    ).bind(sequence, observedAt, applied);
    const insertBucket = (bucketAt: number) => env.DB.prepare(
      `INSERT INTO environment_buckets(
         device_id,bucket_at,sample_count,co2_sum,co2_count,
         temperature_sum,temperature_count,humidity_sum,humidity_count
       ) VALUES('ci-device',?1,1,700,1,24.0,1,49.0,1)`,
    ).bind(bucketAt);

    await env.DB.batch([
      insertSample(1, now - 32 * day, 1),
      insertSample(2, now - 32 * day, 0),
      insertSample(3, now - 3650 * day, 1),
      insertSample(4, now - day, 1),
      insertBucket(now - 8 * day),
      insertBucket(now - 6 * day),
      env.DB.prepare(
        `INSERT INTO octopus_daily_totals(account_number,day,energy_kwh,slot_count,updated_at)
         VALUES('A-123','2020-01-01',0.5,48,?1)`,
      ).bind(now),
    ]);

    await cleanupExpiredData(env, now);

    const samples = await env.DB.prepare(
      "SELECT sequence,bucket_applied FROM environment_samples ORDER BY sequence",
    ).all<{ sequence: number; bucket_applied: number }>();
    expect(samples.results).toEqual([
      { sequence: 1, bucket_applied: 1 },
      { sequence: 2, bucket_applied: 0 },
      { sequence: 3, bucket_applied: 1 },
      { sequence: 4, bucket_applied: 1 },
    ]);

    const buckets = await env.DB.prepare(
      "SELECT bucket_at FROM environment_buckets ORDER BY bucket_at",
    ).all<{ bucket_at: number }>();
    expect(buckets.results).toEqual([
      { bucket_at: now - 8 * day },
      { bucket_at: now - 6 * day },
    ]);

    const octopus = await env.DB.prepare(
      "SELECT day,energy_kwh FROM octopus_daily_totals WHERE account_number='A-123'",
    ).first<{ day: string; energy_kwh: number }>();
    expect(octopus).toMatchObject({ day: "2020-01-01", energy_kwh: 0.5 });
  });
});
