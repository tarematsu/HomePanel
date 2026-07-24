import { applyD1Migrations, env } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import {
  OCTOPUS_COLLECTION_DAYS,
  OCTOPUS_CORRECTION_OVERLAP_DAYS,
  OCTOPUS_HISTORY_FLOOR_MS,
  octopusCollectionStart,
  octopusCompleteStableThroughJst,
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

function completeReadings(range: OctopusRange, value = 0.25): OctopusReading[] {
  const readings: OctopusReading[] = [];
  for (let observedAt = range.from.getTime(); observedAt < range.to.getTime(); observedAt += HALF_HOUR_MS) {
    readings.push({
      supplyPoint: "spin-1",
      startAt: new Date(observedAt).toISOString(),
      value,
    });
  }
  return readings;
}

describe("Octopus daily D1 history", () => {
  it("bootstraps seven complete days without storing half-hour rows", async () => {
    const now = Date.parse("2026-07-10T18:00:00Z");
    const collectionStart = octopusCollectionStart(now);
    const stableThrough = octopusCompleteStableThroughJst(now);
    const requested: OctopusRange[] = [];

    const result = await synchronizeOctopusHistory(env, "A-123", now, async range => {
      requested.push(range);
      return completeReadings(range);
    });

    expect(OCTOPUS_COLLECTION_DAYS).toBe(7);
    expect(requested).toHaveLength(4);
    expect(requested[0]?.from.getTime()).toBe(collectionStart);
    expect(requested.at(-1)?.to.getTime()).toBe(stableThrough);
    expect(requested.every(range => range.from.getTime() >= collectionStart)).toBe(true);
    expect(requested.every(range => range.to.getTime() <= stableThrough)).toBe(true);

    expect(result.completed).toBe(true);
    expect(result.cursorBefore).toBeNull();
    expect(result.cursorAfter).toBe(stableThrough);
    expect(result.fetchFrom).toBe(collectionStart);
    expect(result.stableThrough).toBe(stableThrough);
    expect(result.historyFloor).toBe(OCTOPUS_HISTORY_FLOOR_MS);

    const stored = await env.DB.prepare(
      `SELECT COUNT(*) AS count,MIN(day) AS oldest,MAX(day) AS latest,
              SUM(slot_count) AS slots,SUM(energy_kwh) AS energy
         FROM octopus_daily_totals WHERE account_number=?1`,
    ).bind("A-123").first<{
      count: number; oldest: string; latest: string; slots: number; energy: number;
    }>();
    expect(Number(stored?.count)).toBe(7);
    expect(stored?.oldest).toBe("2026-07-02");
    expect(stored?.latest).toBe("2026-07-08");
    expect(Number(stored?.slots)).toBe(7 * 48);
    expect(Number(stored?.energy)).toBe(7 * 48 * 0.25);

    const cursor = await env.DB.prepare(
      "SELECT stable_through FROM octopus_sync_state WHERE account_number=?1",
    ).bind("A-123").first<{ stable_through: number }>();
    expect(Number(cursor?.stable_through)).toBe(stableThrough);
  });

  it("fetches only one correction day plus newly stable days after bootstrap", async () => {
    const firstNow = Date.parse("2026-07-10T18:00:00Z");
    await synchronizeOctopusHistory(env, "A-incremental", firstNow, async range => completeReadings(range));
    const firstStableThrough = octopusCompleteStableThroughJst(firstNow);

    const secondNow = firstNow + DAY_MS;
    const requested: OctopusRange[] = [];
    const result = await synchronizeOctopusHistory(env, "A-incremental", secondNow, async range => {
      requested.push(range);
      return completeReadings(range);
    });

    expect(OCTOPUS_CORRECTION_OVERLAP_DAYS).toBe(1);
    expect(requested).toHaveLength(1);
    expect(requested[0]?.from.getTime()).toBe(firstStableThrough - DAY_MS);
    expect(requested[0]?.to.getTime()).toBe(firstStableThrough + DAY_MS);
    expect(result.cursorBefore).toBe(firstStableThrough);
    expect(result.cursorAfter).toBe(firstStableThrough + DAY_MS);
    expect(result.fetchFrom).toBe(firstStableThrough - DAY_MS);
    expect(result.stableThrough).toBe(firstStableThrough + DAY_MS);

    const stored = await env.DB.prepare(
      "SELECT COUNT(*) AS count FROM octopus_daily_totals WHERE account_number=?1",
    ).bind("A-incremental").first<{ count: number }>();
    expect(Number(stored?.count)).toBe(8);
  });

  it("keeps the cursor at the first incomplete bootstrap day and retries it", async () => {
    const now = Date.parse("2026-07-10T18:00:00Z");
    const collectionStart = octopusCollectionStart(now);
    const stableThrough = octopusCompleteStableThroughJst(now);
    const missingDayEnd = collectionStart + DAY_MS;

    const incomplete = await synchronizeOctopusHistory(env, "A-gap", now, async range =>
      completeReadings(range).filter(reading => {
        const observedAt = Date.parse(reading.startAt);
        return observedAt < collectionStart || observedAt >= missingDayEnd;
      }));

    expect(incomplete.completed).toBe(false);
    expect(incomplete.cursorAfter).toBe(collectionStart);
    const storedCursor = await env.DB.prepare(
      "SELECT stable_through FROM octopus_sync_state WHERE account_number=?1",
    ).bind("A-gap").first<{ stable_through: number }>();
    expect(Number(storedCursor?.stable_through)).toBe(collectionStart);

    const stillIncomplete = await synchronizeOctopusHistory(env, "A-gap", now, async () => []);
    expect(stillIncomplete.cursorBefore).toBe(collectionStart);
    expect(stillIncomplete.cursorAfter).toBe(collectionStart);
    const monotonicCursor = await env.DB.prepare(
      "SELECT stable_through FROM octopus_sync_state WHERE account_number=?1",
    ).bind("A-gap").first<{ stable_through: number }>();
    expect(Number(monotonicCursor?.stable_through)).toBe(collectionStart);

    const retried: OctopusRange[] = [];
    const repaired = await synchronizeOctopusHistory(env, "A-gap", now, async range => {
      retried.push(range);
      return completeReadings(range);
    });
    expect(retried.some(range => range.from.getTime() <= collectionStart && range.to.getTime() >= missingDayEnd)).toBe(true);
    expect(repaired.completed).toBe(true);
    expect(repaired.cursorAfter).toBe(stableThrough);
  });

  it("uses a compact daily-only schema without supply-point indexes", async () => {
    const raw = await env.DB.prepare(
      "SELECT COUNT(*) AS count FROM sqlite_master WHERE type='table' AND name='octopus_readings'",
    ).first<{ count: number }>();
    expect(Number(raw?.count)).toBe(0);

    const columns = await env.DB.prepare("PRAGMA table_info(octopus_daily_totals)")
      .all<{ name: string; pk: number }>();
    expect(columns.results?.map(column => column.name)).toEqual([
      "account_number", "day", "energy_kwh", "slot_count", "updated_at",
    ]);
    expect(columns.results?.filter(column => column.pk > 0).map(column => column.name)).toEqual([
      "account_number", "day",
    ]);

    const namedIndexes = await env.DB.prepare(
      `SELECT name FROM sqlite_master
        WHERE type='index' AND tbl_name='octopus_daily_totals' AND name NOT LIKE 'sqlite_autoindex_%'`,
    ).all<{ name: string }>();
    expect(namedIndexes.results).toEqual([]);
  });

  it("aligns the storage boundary to a fully stable JST day", () => {
    const now = Date.parse("2026-07-10T18:17:42Z");
    expect(new Date(octopusStableCutoffJst(now)).toISOString()).toBe("2026-07-08T18:00:00.000Z");
    expect(new Date(octopusCompleteStableThroughJst(now)).toISOString()).toBe("2026-07-08T15:00:00.000Z");
    expect(new Date(octopusCollectionStart(now)).toISOString()).toBe("2026-07-01T15:00:00.000Z");
  });
});
