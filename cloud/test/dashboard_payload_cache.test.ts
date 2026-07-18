import { describe, expect, it } from "vitest";
import { dashboardPayload, type StateRow } from "../src/snapshot";

function state(overrides: Partial<StateRow> = {}): StateRow {
  return {
    source: "weather",
    version: 987_654,
    payload: JSON.stringify({ temperature: 21 }),
    observed_at: 1,
    fetched_at: 2,
    last_success_at: 2,
    status: "ok",
    error: null,
    content_hash: "cache-test-a",
    ...overrides,
  };
}

describe("dashboard source payload cache", () => {
  it("reuses unchanged transformed payloads and invalidates metadata or content changes", () => {
    const row = state();
    const first = dashboardPayload({ weather: row });
    const second = dashboardPayload({ weather: row });
    expect(second.weather).toBe(first.weather);
    expect(second.weather).toMatchObject({
      temperature: 21,
      __status: "ok",
      __error: null,
      __lastSuccessAt: null,
      __version: 987_654,
    });

    const stale = dashboardPayload({
      weather: state({ status: "stale", error: "late", last_success_at: 3 }),
    });
    expect(stale.weather).not.toBe(first.weather);
    expect(stale.weather).toMatchObject({
      temperature: 21,
      __status: "stale",
      __error: "late",
      __lastSuccessAt: 3,
    });

    const changed = dashboardPayload({
      weather: state({
        version: 987_655,
        payload: JSON.stringify({ temperature: 22 }),
        content_hash: "cache-test-b",
      }),
    });
    expect(changed.weather).not.toBe(first.weather);
    expect(changed.weather).toMatchObject({ temperature: 22, __version: 987_655 });
  });
});
