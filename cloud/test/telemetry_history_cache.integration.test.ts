import { applyD1Migrations, env } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { readState, updateState } from "../src/snapshot";
import {
  invalidateEnvironmentStateCache,
  mergeEnvironmentRows,
} from "../src/telemetry_history";
import type { EnvironmentHistoryRow } from "../src/telemetry_bucket";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

const DEVICE_A = "homepanel-device";
const DEVICE_B = "homepanel-backup";

function point(t: number, co2: number): EnvironmentHistoryRow {
  return { t, co2, temperature: 21.5, humidity: 48.25 };
}

function payloadFor(
  selectedDeviceId: string,
  devices: Record<string, EnvironmentHistoryRow[]>,
): Record<string, unknown> {
  const mapped = Object.fromEntries(Object.entries(devices).map(([deviceId, history]) => [
    deviceId,
    { deviceId, bucketMinutes: 5, history },
  ]));
  return {
    deviceId: selectedDeviceId,
    bucketMinutes: 5,
    history: devices[selectedDeviceId] ?? [],
    devices: mapped,
  };
}

function trackingEnv(queries: string[]): typeof env {
  const db = {
    prepare(query: string): D1PreparedStatement {
      queries.push(query);
      return env.DB.prepare(query);
    },
  } as D1Database;
  return { DB: db } as typeof env;
}

function environmentReads(queries: readonly string[]): number {
  return queries.filter(query =>
    query.includes("FROM current_state WHERE source = ?1")
  ).length;
}

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
});

describe("environment state row cache", () => {
  it("reuses the cached row across successful telemetry merges", async () => {
    const now = Date.now();
    const first = point(now - 10 * 60_000, 600);
    const second = point(now - 5 * 60_000, 610);
    const third = point(now, 620);
    await updateState(env, {
      source: "environment",
      observedAt: first.t,
      payload: payloadFor(DEVICE_A, { [DEVICE_A]: [first] }),
    });

    const queries: string[] = [];
    const tracked = trackingEnv(queries);
    invalidateEnvironmentStateCache(tracked.DB);
    await mergeEnvironmentRows(tracked, DEVICE_A, [second], second.t);
    await mergeEnvironmentRows(tracked, DEVICE_A, [third], third.t);

    expect(environmentReads(queries)).toBe(1);
    const state = await readState(env, "environment");
    expect(state?.version).toBe(3);
    const payload = JSON.parse(state!.payload) as {
      devices: Record<string, { history: EnvironmentHistoryRow[] }>;
    };
    expect(payload.devices[DEVICE_A]?.history.map(row => row.t)).toEqual([
      first.t,
      second.t,
      third.t,
    ]);
  });

  it("reloads after a competing writer and preserves its device history", async () => {
    const now = Date.now();
    const first = point(now - 15 * 60_000, 600);
    const second = point(now - 10 * 60_000, 610);
    const external = point(now - 5 * 60_000, 700);
    const third = point(now, 620);
    await updateState(env, {
      source: "environment",
      observedAt: first.t,
      payload: payloadFor(DEVICE_A, { [DEVICE_A]: [first] }),
    });

    const queries: string[] = [];
    const tracked = trackingEnv(queries);
    invalidateEnvironmentStateCache(tracked.DB);
    await mergeEnvironmentRows(tracked, DEVICE_A, [second], second.t);

    const current = await readState(env, "environment");
    const currentPayload = JSON.parse(current!.payload) as {
      devices: Record<string, { history: EnvironmentHistoryRow[] }>;
    };
    await updateState(env, {
      source: "environment",
      observedAt: external.t,
      payload: payloadFor(DEVICE_A, {
        [DEVICE_A]: currentPayload.devices[DEVICE_A]!.history,
        [DEVICE_B]: [external],
      }),
    });

    await mergeEnvironmentRows(tracked, DEVICE_A, [third], third.t);

    expect(environmentReads(queries)).toBe(2);
    const state = await readState(env, "environment");
    const payload = JSON.parse(state!.payload) as {
      devices: Record<string, { history: EnvironmentHistoryRow[] }>;
    };
    expect(payload.devices[DEVICE_A]?.history.at(-1)?.t).toBe(third.t);
    expect(payload.devices[DEVICE_B]?.history).toEqual([external]);
  });
});
