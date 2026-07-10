import { describe, expect, it } from "vitest";

import {
  evaluateStationheadHealth,
  stationheadHealthUrl,
} from "../src/stationhead_health";

describe("Stationhead external health monitoring", () => {
  it("derives the health endpoint from the playback monitor URL", () => {
    expect(stationheadHealthUrl({
      STATIONHEAD_MONITOR_URL: "https://monitor.example/api/playback?channel=buddy46",
    })).toBe("https://monitor.example/api/health");
  });

  it("prefers an explicitly configured health endpoint", () => {
    expect(stationheadHealthUrl({
      STATIONHEAD_HEALTH_URL: "https://health.example/custom",
      STATIONHEAD_MONITOR_URL: "https://monitor.example/api/playback",
    })).toBe("https://health.example/custom");
  });

  it("marks a fresh healthy collector as healthy", () => {
    const now = 2_000_000;
    const result = evaluateStationheadHealth({
      ok: true,
      collector_health_ok: true,
      collector_health_stale: false,
      collector_last_success_at: now - 60_000,
      collector_health_age_ms: 60_000,
      collector_health_stale_after_ms: 900_000,
    }, true, 200, now, 900_000);

    expect(result.healthy).toBe(true);
    expect(result.reason).toBeNull();
  });

  it("marks a stale collector as unhealthy even when the endpoint responds", () => {
    const result = evaluateStationheadHealth({
      ok: true,
      collector_health_ok: false,
      collector_health_stale: true,
      collector_health_age_ms: 1_000_000,
      collector_health_stale_after_ms: 900_000,
    }, false, 503, 2_000_000, 900_000);

    expect(result.healthy).toBe(false);
    expect(result.reachable).toBe(true);
    expect(result.reason).toContain("HTTP 503");
  });
});
