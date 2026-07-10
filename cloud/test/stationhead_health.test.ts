import { describe, expect, it } from "vitest";

import {
  alertTransitionKey,
  evaluateStationheadHealth,
  stationheadHealthUrl,
  type StationheadHealthSnapshot,
} from "../src/stationhead_health";

function snapshot(overrides: Partial<StationheadHealthSnapshot> = {}): StationheadHealthSnapshot {
  return {
    configured: true,
    reachable: true,
    healthy: true,
    statusCode: 200,
    sampledAt: 2_000_000,
    lastRunAt: 1_990_000,
    lastSuccessAt: 1_980_000,
    ageMs: 20_000,
    staleAfterMs: 900_000,
    reason: null,
    alertConfigured: true,
    alertPending: false,
    recoveryPending: false,
    alertEventKey: null,
    ...overrides,
  };
}

describe("Stationhead external health monitoring", () => {
  it("derives the health endpoint from the playback monitor URL", () => {
    expect(stationheadHealthUrl({
      STATIONHEAD_MONITOR_URL: "https://monitor.example/api/playback?channel=buddy46",
    })).toBe("https://monitor.example/api/health");
  });

  it("handles a trailing slash when deriving the health endpoint", () => {
    expect(stationheadHealthUrl({
      STATIONHEAD_MONITOR_URL: "https://monitor.example/api/playback/",
    })).toBe("https://monitor.example/api/health");
  });

  it("prefers an explicitly configured health endpoint", () => {
    expect(stationheadHealthUrl({
      STATIONHEAD_HEALTH_URL: "https://health.example/custom",
      STATIONHEAD_MONITOR_URL: "https://monitor.example/api/playback",
    })).toBe("https://health.example/custom");
  });

  it("marks the real Stationhead health response as healthy", () => {
    const now = 2_000_000;
    const result = evaluateStationheadHealth({
      ok: true,
      collector_stale: false,
      collector_last_success_at: now - 60_000,
      collector_age_ms: 60_000,
      collector_stale_after_ms: 3_600_000,
    }, true, 200, now, 900_000);

    expect(result.healthy).toBe(true);
    expect(result.reason).toBeNull();
    expect(result.staleAfterMs).toBe(900_000);
  });

  it("keeps compatibility with the older collector_health field names", () => {
    const now = 2_000_000;
    const result = evaluateStationheadHealth({
      collector_health_ok: true,
      collector_health_stale: false,
      collector_last_success_at: now - 60_000,
      collector_health_age_ms: 60_000,
    }, true, 200, now, 900_000);

    expect(result.healthy).toBe(true);
  });

  it("rejects a successful HTTP response without an explicit health result", () => {
    const result = evaluateStationheadHealth({}, true, 200, 2_000_000, 900_000);

    expect(result.reachable).toBe(true);
    expect(result.healthy).toBe(false);
    expect(result.reason).toContain("explicit health status");
  });

  it("does not convert null collector timestamps into the Unix epoch", () => {
    const result = evaluateStationheadHealth({
      ok: false,
      collector_last_run_at: null,
      collector_last_success_at: null,
      collector_age_ms: null,
    }, false, 503, 2_000_000, 900_000);

    expect(result.lastRunAt).toBeNull();
    expect(result.lastSuccessAt).toBeNull();
    expect(result.ageMs).toBeNull();
    expect(result.healthy).toBe(false);
  });

  it("marks a stale collector as unhealthy even when the endpoint responds", () => {
    const result = evaluateStationheadHealth({
      ok: false,
      collector_stale: true,
      collector_age_ms: 1_000_000,
      collector_stale_after_ms: 900_000,
      collector_last_success_at: 1_000_000,
    }, false, 503, 2_000_000, 900_000);

    expect(result.healthy).toBe(false);
    expect(result.reachable).toBe(true);
    expect(result.reason).toContain("HTTP 503");
  });

  it("keeps the same recovery alert key across recovery retries while success timestamps advance", () => {
    const previous = snapshot({
      recoveryPending: true,
      alertEventKey: "homepanel-stationhead-recovered-900000",
    });
    const current = snapshot({
      sampledAt: 2_060_000,
      lastRunAt: 2_050_000,
      lastSuccessAt: 2_040_000,
    });

    expect(alertTransitionKey(current, previous, true))
      .toBe("homepanel-stationhead-recovered-900000");
  });

  it("does not reuse a failed outage key for a later recovery notification", () => {
    const previous = snapshot({
      healthy: false,
      statusCode: 503,
      alertPending: true,
      alertEventKey: "homepanel-stationhead-down-1980000",
    });
    const current = snapshot({
      sampledAt: 2_060_000,
      lastSuccessAt: 2_040_000,
    });

    expect(alertTransitionKey(current, previous, true))
      .toBe("homepanel-stationhead-recovered-1980000");
  });

  it("does not reuse a failed recovery key for a later outage notification", () => {
    const previous = snapshot({
      recoveryPending: true,
      alertEventKey: "homepanel-stationhead-recovered-1980000",
    });
    const current = snapshot({
      healthy: false,
      statusCode: 503,
      sampledAt: 2_060_000,
      lastSuccessAt: 2_040_000,
    });

    expect(alertTransitionKey(current, previous, false))
      .toBe("homepanel-stationhead-down-2040000");
  });
});
