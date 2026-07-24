import { describe, expect, it } from "vitest";
import {
  buildOctopusDailyProfile,
  completeDayProfileRanges,
  projectOctopusMonthlyUsage,
  type OctopusProfileRanges,
} from "../src/octopus_source";
import type { OctopusDailyTotal } from "../src/octopus_history";

const JST_MS = 9 * 60 * 60_000;
const DAY_MS = 86_400_000;

function dayKey(timestampMs: number): string {
  return new Date(timestampMs + JST_MS).toISOString().slice(0, 10);
}

function dailyTotals(
  startMs: number,
  days: number,
  energyKwh: number,
  slotCount = 48,
): OctopusDailyTotal[] {
  return Array.from({ length: days }, (_, day) => ({
    day: dayKey(startMs + day * DAY_MS),
    energyKwh,
    slotCount,
  }));
}

function profileRanges(): OctopusProfileRanges {
  return {
    previousStart: new Date("2026-06-24T15:00:00.000Z"),
    previousEnd: new Date("2026-07-01T15:00:00.000Z"),
    currentStart: new Date("2026-07-01T15:00:00.000Z"),
    currentEnd: new Date("2026-07-08T15:00:00.000Z"),
  };
}

describe("Octopus complete-day profile", () => {
  it("aligns comparison blocks to the last fully stable JST day", () => {
    const now = Date.parse("2026-07-10T18:17:00Z");
    const ranges = completeDayProfileRanges(now);

    expect(ranges.currentStart.toISOString()).toBe("2026-07-01T15:00:00.000Z");
    expect(ranges.currentEnd.toISOString()).toBe("2026-07-08T15:00:00.000Z");
    expect(ranges.previousStart.toISOString()).toBe("2026-06-24T15:00:00.000Z");
    expect(ranges.previousEnd.toISOString()).toBe("2026-07-01T15:00:00.000Z");

    const totals = [
      ...dailyTotals(ranges.previousStart.getTime(), 7, 9.6),
      ...dailyTotals(ranges.currentStart.getTime(), 7, 19.2),
      ...dailyTotals(ranges.currentEnd.getTime(), 2, 999),
    ];
    const profile = buildOctopusDailyProfile(totals, ranges);

    expect(profile).toHaveLength(7);
    expect(profile[0]).toEqual({
      day: "木",
      currentTotal: 19.2,
      previousTotal: 9.6,
      currentComplete: true,
      previousComplete: true,
    });
    expect(profile[6]).toEqual({
      day: "水",
      currentTotal: 19.2,
      previousTotal: 9.6,
      currentComplete: true,
      previousComplete: true,
    });
  });

  it("hides only an incomplete daily aggregate", () => {
    const ranges = profileRanges();
    const totals = [
      ...dailyTotals(ranges.previousStart.getTime(), 7, 9.6),
      ...dailyTotals(ranges.currentStart.getTime(), 6, 19.2),
      ...dailyTotals(ranges.currentStart.getTime() + 6 * DAY_MS, 1, 19.2, 47),
    ];

    const profile = buildOctopusDailyProfile(totals, ranges);
    expect(profile.slice(0, 6).every(point => point.currentComplete && point.currentTotal === 19.2)).toBe(true);
    expect(profile[6]!.currentComplete).toBe(false);
    expect(profile[6]!.currentTotal).toBeNull();
    expect(profile.every(point => point.previousComplete && point.previousTotal === 9.6)).toBe(true);
  });

  it("does not treat a partial daily row as complete", () => {
    const ranges = profileRanges();
    const totals = dailyTotals(ranges.currentStart.getTime(), 7, 1, 1);

    const profile = buildOctopusDailyProfile(totals, ranges);
    expect(profile.every(point => point.currentTotal === null)).toBe(true);
    expect(profile.every(point => point.currentComplete === false)).toBe(true);
  });

  it("projects from covered complete days instead of treating gaps as zero usage", () => {
    expect(projectOctopusMonthlyUsage(60, 6, 30)).toBe(300);
    expect(projectOctopusMonthlyUsage(60, 0, 30)).toBe(0);
    expect(projectOctopusMonthlyUsage(Number.NaN, 6, 30)).toBe(0);
  });
});
