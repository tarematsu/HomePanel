import { applyD1Migrations, env } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { cleanupExpiredData } from "../src/scheduler";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
});

describe("D1 retention", () => {
  it("removes expired power samples while retaining Octopus daily history", async () => {
    const now = Date.now();
    const day = 86_400_000;
    await env.DB.batch([
      env.DB.prepare(
        `INSERT INTO power_samples(source,observed_at,watts,payload)
         VALUES('switchbot',?1,10,NULL)`,
      ).bind(now - 100 * day),
      env.DB.prepare(
        `INSERT INTO power_samples(source,observed_at,watts,payload)
         VALUES('switchbot',?1,20,NULL)`,
      ).bind(now - day),
      env.DB.prepare(
        `INSERT INTO octopus_daily_totals(account_number,day,energy_kwh,slot_count,updated_at)
         VALUES('A-123','2020-01-01',0.5,48,?1)`,
      ).bind(now),
    ]);

    await cleanupExpiredData(env, now);

    const power = await env.DB.prepare(
      "SELECT watts FROM power_samples ORDER BY observed_at",
    ).all<{ watts: number }>();
    expect(power.results).toEqual([{ watts: 20 }]);

    const octopus = await env.DB.prepare(
      "SELECT day,energy_kwh FROM octopus_daily_totals WHERE account_number='A-123'",
    ).first<{ day: string; energy_kwh: number }>();
    expect(octopus).toMatchObject({ day: "2020-01-01", energy_kwh: 0.5 });

    const retired = await env.DB.prepare(
      `SELECT name FROM sqlite_schema
        WHERE type='table' AND name IN ('environment_samples','environment_buckets')`,
    ).all<{ name: string }>();
    expect(retired.results).toEqual([]);
  });
});
