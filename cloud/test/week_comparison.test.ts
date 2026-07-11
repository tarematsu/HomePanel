import { describe, expect, it } from "vitest";
import {
  alignedWeekComparison,
  isoWeekInfoJst,
} from "../src/week_comparison";

describe("energy week comparison", () => {
  it("aligns the current and previous ISO week by weekday in JST", () => {
    const comparison = alignedWeekComparison(Date.parse("2026-07-10T18:00:00Z"));

    expect(comparison.current).toEqual({ year: 2026, week: 28, weekday: 6 });
    expect(comparison.previousWeek).toEqual({ year: 2026, week: 27, weekday: 6 });
    expect(comparison.currentWeekStart.toISOString()).toBe("2026-07-05T15:00:00.000Z");
    expect(comparison.currentWeekEnd.toISOString()).toBe("2026-07-12T15:00:00.000Z");
    expect(comparison.previousWeekStart.toISOString()).toBe("2026-06-28T15:00:00.000Z");
    expect(comparison.previousWeekEnd.toISOString()).toBe("2026-07-05T15:00:00.000Z");

    for (let index = 0; index < 7; index += 1) {
      const currentDay = comparison.currentWeekStart.getTime() + index * 86_400_000;
      const previousDay = comparison.previousWeekStart.getTime() + index * 86_400_000;
      expect(isoWeekInfoJst(currentDay).weekday).toBe(index + 1);
      expect(isoWeekInfoJst(previousDay).weekday).toBe(index + 1);
    }
  });

  it("crosses the ISO week-year boundary correctly", () => {
    const comparison = alignedWeekComparison(Date.parse("2026-01-01T03:00:00Z"));

    expect(comparison.current).toEqual({ year: 2026, week: 1, weekday: 4 });
    expect(comparison.previousWeek).toEqual({ year: 2025, week: 52, weekday: 4 });
    expect(comparison.currentWeekStart.toISOString()).toBe("2025-12-28T15:00:00.000Z");
    expect(comparison.previousWeekStart.toISOString()).toBe("2025-12-21T15:00:00.000Z");
  });

  it("uses JST when UTC and local dates fall on different weekdays", () => {
    expect(isoWeekInfoJst(Date.parse("2026-07-05T16:00:00Z"))).toEqual({
      year: 2026,
      week: 28,
      weekday: 1,
    });
  });
});
