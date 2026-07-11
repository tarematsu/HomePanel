import { applyD1Migrations, env } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import {
  OCTOPUS_HISTORY_FLOOR_MS,
  octopusStableCutoffJst,
  synchronizeOctopusHistory,
  type OctopusRange,
  type OctopusReading,
} from "../src/octopus_history";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
});

function readingInside(range: OctopusRange): OctopusReading {
  return {
    supplyPoint: "spin-1",
    startAt: new Date(range.from.getTime() + 30 * 60_000).toISOString(),
    value: 0.25,
  };
}

describe("Octopus D1 history", () => {
  it("moves the backfill cursor backward and never stores the latest 48 hours", async () => {
    const now = Date.parse("2026-07-10T18:00:00Z");
    const stableCutoff = octopusStableCutoffJst(now);
    expect(new Date(stableCutoff).toISOString()).toBe("2026-07-08T18:00:00.000Z");
    const comparison = {
      from: new Date("2026-06-28T15:00:00.000Z"),
      to: new Date("2026-07-05T15:00:00.000Z"),
    };
    const requested: OctopusRange[] = [];
    const fetchRange = async (range: OctopusRange): Promise<OctopusReading[]> => {
      requested.push(range);
      return [readingInside(range)];
    };

    const first = await synchronizeOctopusHistory(
      env,
      "A-123",
      now,
      "iso-week:2026-W27",
      comparison,
      fetchRange,
    );
    expect(first.completed).toBe(false);
    expect(first.historyFloor).toBe(OCTOPUS_HISTORY_FLOOR_MS);
    expect(first.liveReadings.length).toBeGreaterThan(0);
    expect(first.liveReadings.every(reading => Date.parse(reading.startAt) >= stableCutoff)).toBe(true);

    const stored = await env.DB.prepare(
      "SELECT COUNT(*) AS count,MIN(observed_at) AS oldest,MAX(observed_at) AS latest FROM octopus_readings",
    ).first<{ count: number; oldest: number; latest: number }>();
    expect(Number(stored?.count)).toBeGreaterThan(20);
    expect(Number(stored?.latest)).toBeLessThan(stableCutoff);
    expect(Number(stored?.oldest)).toBeLessThan(stableCutoff - 30 * 86_400_000);
    expect(Number(stored?.oldest)).toBeGreaterThanOrEqual(OCTOPUS_HISTORY_FLOOR_MS);

    const firstCursor = first.cursorBefore;
    requested.length = 0;
    const second = await synchronizeOctopusHistory(
      env,
      "A-123",
      now,
      "iso-week:2026-W27",
      comparison,
      fetchRange,
    );
    expect(second.cursorBefore).toBe(firstCursor - 30 * 86_400_000);
    expect(requested.some(range => range.from.getTime() === comparison.from.getTime())).toBe(false);

    const marked = await env.DB.prepare(
      "SELECT COUNT(*) AS count FROM octopus_sync_ranges WHERE account_number=?1 AND range_key=?2",
    ).bind("A-123", "iso-week:2026-W27").first<{ count: number }>();
    expect(marked?.count).toBe(1);
  });

  it("rounds the rolling cutoff down to a half-hour reading boundary", () => {
    const now = Date.parse("2026-07-10T18:17:42Z");
    expect(new Date(octopusStableCutoffJst(now)).toISOString()).toBe("2026-07-08T18:00:00.000Z");
  });

  it("never requests or retains readings older than November 2025", async () => {
    const now = Date.parse("2026-07-10T18:00:00Z");
    const comparison = {
      from: new Date("2026-06-28T15:00:00.000Z"),
      to: new Date("2026-07-05T15:00:00.000Z"),
    };
    const requested: OctopusRange[] = [];
    const fetchRange = async (range: OctopusRange): Promise<OctopusReading[]> => {
      requested.push(range);
      return [readingInside(range)];
    };

    let result = await synchronizeOctopusHistory(env, "A-floor", now, "week", comparison, fetchRange);
    for (let run = 1; run < 12 && !result.completed; run += 1) {
      result = await synchronizeOctopusHistory(env, "A-floor", now, "week", comparison, fetchRange);
    }

    expect(result.completed).toBe(true);
    expect(result.cursorBefore).toBe(OCTOPUS_HISTORY_FLOOR_MS);
    expect(requested.every(range => range.from.getTime() >= OCTOPUS_HISTORY_FLOOR_MS)).toBe(true);

    await env.DB.prepare(
      `INSERT INTO octopus_readings(account_number,supply_point,observed_at,energy_kwh,updated_at)
       VALUES('A-floor','old-spin',?1,1.0,?2)`,
    ).bind(OCTOPUS_HISTORY_FLOOR_MS - 86_400_000, now).run();
    await synchronizeOctopusHistory(env, "A-floor", now, "week", comparison, fetchRange);
    const oldRows = await env.DB.prepare(
      "SELECT COUNT(*) AS count FROM octopus_readings WHERE account_number='A-floor' AND observed_at<?1",
    ).bind(OCTOPUS_HISTORY_FLOOR_MS).first<{ count: number }>();
    expect(oldRows?.count).toBe(0);
  });

  it("stops only after a long empty tail instead of a single missing day", async () => {
    const now = Date.parse("2026-07-10T18:00:00Z");
    const comparison = {
      from: new Date("2026-06-28T15:00:00.000Z"),
      to: new Date("2026-07-05T15:00:00.000Z"),
    };
    const stableCutoff = octopusStableCutoffJst(now);
    const recentStart = stableCutoff - 7 * 86_400_000;
    const fetchRange = async (range: OctopusRange): Promise<OctopusReading[]> => {
      if (range.from.getTime() >= recentStart || range.from.getTime() === comparison.from.getTime()) {
        return [readingInside(range)];
      }
      return [];
    };

    const first = await synchronizeOctopusHistory(env, "A-456", now, "week", comparison, fetchRange);
    expect(first.completed).toBe(false);
    const second = await synchronizeOctopusHistory(env, "A-456", now, "week", comparison, fetchRange);
    expect(second.completed).toBe(false);
    const third = await synchronizeOctopusHistory(env, "A-456", now, "week", comparison, fetchRange);
    expect(third.completed).toBe(true);

    const state = await env.DB.prepare(
      "SELECT consecutive_empty_days,completed FROM octopus_backfill_state WHERE account_number=?1",
    ).bind("A-456").first<{ consecutive_empty_days: number; completed: number }>();
    expect(Number(state?.consecutive_empty_days)).toBeGreaterThanOrEqual(62);
    expect(state?.completed).toBe(1);
  });
});
