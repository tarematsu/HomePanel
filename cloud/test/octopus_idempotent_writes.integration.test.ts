import { applyD1Migrations, env } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import {
  synchronizeOctopusHistory,
  type OctopusRange,
  type OctopusReading,
} from "../src/octopus_history";
import { resetD1TestDatabase } from "./d1_test_utils";

 type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

const HALF_HOUR_MS = 30 * 60_000;

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
});

describe("Octopus idempotent daily writes", () => {
  it("keeps updated_at stable for unchanged totals and updates a corrected overlap day", async () => {
    const now = Date.parse("2026-07-10T18:00:00Z");
    const correctedDayStart = Date.parse("2026-07-07T15:00:00Z");
    let correctedDaySlotValue = 0.25;
    const fetchRange = async (range: OctopusRange): Promise<OctopusReading[]> => {
      const readings: OctopusReading[] = [];
      for (let observedAt = range.from.getTime(); observedAt < range.to.getTime(); observedAt += HALF_HOUR_MS) {
        readings.push({
          supplyPoint: "spin-idempotent",
          startAt: new Date(observedAt).toISOString(),
          value: observedAt >= correctedDayStart && observedAt < correctedDayStart + 48 * HALF_HOUR_MS
            ? correctedDaySlotValue
            : 0.1,
        });
      }
      return readings;
    };

    await synchronizeOctopusHistory(env, "A-idempotent", now, fetchRange);
    const initial = await env.DB.prepare(
      `SELECT energy_kwh,updated_at FROM octopus_daily_totals
        WHERE account_number=?1 AND day=?2`,
    ).bind("A-idempotent", "2026-07-08")
      .first<{ energy_kwh: number; updated_at: number }>();
    expect(initial).toEqual({ energy_kwh: 12, updated_at: now });

    const unchangedNow = now + 60_000;
    await synchronizeOctopusHistory(env, "A-idempotent", unchangedNow, fetchRange);
    const unchanged = await env.DB.prepare(
      `SELECT energy_kwh,updated_at FROM octopus_daily_totals
        WHERE account_number=?1 AND day=?2`,
    ).bind("A-idempotent", "2026-07-08")
      .first<{ energy_kwh: number; updated_at: number }>();
    expect(unchanged).toEqual({ energy_kwh: 12, updated_at: now });

    correctedDaySlotValue = 0.5;
    const correctedNow = unchangedNow + 60_000;
    await synchronizeOctopusHistory(env, "A-idempotent", correctedNow, fetchRange);
    const corrected = await env.DB.prepare(
      `SELECT energy_kwh,updated_at FROM octopus_daily_totals
        WHERE account_number=?1 AND day=?2`,
    ).bind("A-idempotent", "2026-07-08")
      .first<{ energy_kwh: number; updated_at: number }>();
    expect(corrected).toEqual({ energy_kwh: 24, updated_at: correctedNow });
  });
});
