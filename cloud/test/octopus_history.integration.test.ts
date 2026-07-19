import { applyD1Migrations, env } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import {
  OCTOPUS_COLLECTION_DAYS,
  OCTOPUS_HISTORY_FLOOR_MS,
  octopusCollectionStart,
  octopusStableCutoffJst,
  synchronizeOctopusHistory,
  type OctopusRange,
  type OctopusReading,
} from "../src/octopus_history";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

const HALF_HOUR_MS = 30 * 60_000;
const DAY_MS = 86_400_000;

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
});

function completeReadings(range: OctopusRange): OctopusReading[] {
  const readings: OctopusReading[] = [];
  for (let observedAt = range.from.getTime(); observedAt < range.to.getTime(); observedAt += HALF_HOUR_MS) {
    readings.push({
      supplyPoint: "spin-1",
      startAt: new Date(observedAt).toISOString(),
      value: 0.25,
    });
  }
  return readings;
}

describe("Octopus D1 history", () => {
  it("refreshes only the latest seven days while retaining older stored history", async () => {
    const now = Date.parse("2026-07-10T18:00:00Z");
    const collectionStart = octopusCollectionStart(now);
    const stableCutoff = octopusStableCutoffJst(now);
    const oldStoredAt = Date.parse("2026-06-15T00:00:00Z");
    await env.DB.prepare(
      `INSERT INTO octopus_readings(account_number,supply_point,observed_at,energy_kwh,updated_at)
       VALUES(?1,?2,?3,?4,?5)`,
    ).bind("A-123", "old-spin", oldStoredAt, 1.5, now).run();

    const requested: OctopusRange[] = [];
    const fetchRange = async (range: OctopusRange): Promise<OctopusReading[]> => {
      requested.push(range);
      return completeReadings(range);
    };
    const comparison = {
      from: new Date("2026-06-25T15:00:00.000Z"),
      to: new Date("2026-07-09T15:00:00.000Z"),
    };

    const result = await synchronizeOctopusHistory(
      env,
      "A-123",
      now,
      "complete-profile-v2:2026-06-26:2026-07-09",
      comparison,
      fetchRange,
    );

    expect(OCTOPUS_COLLECTION_DAYS).toBe(7);
    expect(requested).toHaveLength(4);
    expect(requested[0]?.from.getTime()).toBe(collectionStart);
    expect(requested.at(-1)?.to.getTime()).toBe(now);
    expect(requested.every(range => range.from.getTime() >= collectionStart)).toBe(true);
    expect(requested.every(range => range.to.getTime() <= now)).toBe(true);
    expect(requested.some(range => range.from.getTime() === comparison.from.getTime())).toBe(false);

    expect(result.completed).toBe(true);
    expect(result.cursorBefore).toBe(collectionStart);
    expect(result.historyFloor).toBe(OCTOPUS_HISTORY_FLOOR_MS);
    expect(result.liveReadings).toHaveLength(2 * 48);
    expect(result.liveReadings.every(reading => {
      const observedAt = Date.parse(reading.startAt);
      return observedAt >= stableCutoff && observedAt < now;
    })).toBe(true);

    const oldRow = await env.DB.prepare(
      "SELECT energy_kwh FROM octopus_readings WHERE account_number=?1 AND supply_point=?2 AND observed_at=?3",
    ).bind("A-123", "old-spin", oldStoredAt).first<{ energy_kwh: number }>();
    expect(oldRow?.energy_kwh).toBe(1.5);

    const recent = await env.DB.prepare(
      `SELECT COUNT(*) AS count,MIN(observed_at) AS oldest,MAX(observed_at) AS latest
         FROM octopus_readings
        WHERE account_number=?1 AND supply_point='spin-1'`,
    ).bind("A-123").first<{ count: number; oldest: number; latest: number }>();
    expect(Number(recent?.count)).toBe(5 * 48);
    expect(Number(recent?.oldest)).toBe(collectionStart);
    expect(Number(recent?.latest)).toBe(stableCutoff - HALF_HOUR_MS);
  });

  it("removes only rows below the existing history floor", async () => {
    const now = Date.parse("2026-07-10T18:00:00Z");
    await env.DB.prepare(
      `INSERT INTO octopus_readings(account_number,supply_point,observed_at,energy_kwh,updated_at)
       VALUES(?1,?2,?3,?4,?5)`,
    ).bind("A-floor", "old-spin", OCTOPUS_HISTORY_FLOOR_MS - DAY_MS, 1.0, now).run();

    await synchronizeOctopusHistory(
      env,
      "A-floor",
      now,
      "unused",
      { from: new Date(0), to: new Date(0) },
      async () => [],
    );

    const oldRows = await env.DB.prepare(
      "SELECT COUNT(*) AS count FROM octopus_readings WHERE account_number=?1 AND observed_at<?2",
    ).bind("A-floor", OCTOPUS_HISTORY_FLOOR_MS).first<{ count: number }>();
    expect(oldRows?.count).toBe(0);
  });

  it("rounds collection and stability boundaries down to half-hour slots", () => {
    const now = Date.parse("2026-07-10T18:17:42Z");
    expect(new Date(octopusStableCutoffJst(now)).toISOString()).toBe("2026-07-08T18:00:00.000Z");
    expect(new Date(octopusCollectionStart(now)).toISOString()).toBe("2026-07-03T18:00:00.000Z");
  });
});
