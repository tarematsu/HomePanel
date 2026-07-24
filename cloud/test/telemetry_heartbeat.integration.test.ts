import { applyD1Migrations, env } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { telemetryHeartbeatReturningStatement } from "../src/telemetry_heartbeat";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

type HeartbeatRow = {
  last_seen_at: number;
  app_version: string | null;
  stationhead_ok: number | null;
  outbox_count: number | null;
  last_sequence: number;
};

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
});

describe("compact telemetry heartbeat", () => {
  it("does not rewrite D1 for sequence-only changes inside six hours", async () => {
    const startedAt = 1_800_000_000_000;
    const first = await telemetryHeartbeatReturningStatement(
      env,
      "homepanel-device",
      startedAt,
      "2.13.0",
      1,
      0,
      1,
    ).first<{ last_sequence: number }>();
    expect(first?.last_sequence).toBe(1);

    const suppressed = await telemetryHeartbeatReturningStatement(
      env,
      "homepanel-device",
      startedAt + 2 * 60 * 60_000,
      "2.13.0",
      1,
      0,
      2,
    ).first<{ last_sequence: number }>();
    expect(suppressed).toBeNull();

    const unchanged = await env.DB.prepare(
      `SELECT last_seen_at,app_version,stationhead_ok,outbox_count,last_sequence
         FROM device_heartbeats WHERE device_id=?1`,
    ).bind("homepanel-device").first<HeartbeatRow>();
    expect(unchanged).toEqual({
      last_seen_at: startedAt,
      app_version: "2.13.0",
      stationhead_ok: 1,
      outbox_count: 0,
      last_sequence: 1,
    });

    const refreshed = await telemetryHeartbeatReturningStatement(
      env,
      "homepanel-device",
      startedAt + 6 * 60 * 60_000,
      "2.13.0",
      1,
      0,
      3,
    ).first<{ last_sequence: number }>();
    expect(refreshed?.last_sequence).toBe(3);
  });

  it("writes immediately when diagnostic metadata changes", async () => {
    const startedAt = 1_800_000_000_000;
    await telemetryHeartbeatReturningStatement(
      env,
      "homepanel-device",
      startedAt,
      "2.13.0",
      1,
      0,
      1,
    ).first();

    const changed = await telemetryHeartbeatReturningStatement(
      env,
      "homepanel-device",
      startedAt + 60_000,
      "2.14.0",
      0,
      2,
      2,
    ).first<{ last_sequence: number }>();
    expect(changed?.last_sequence).toBe(2);

    const row = await env.DB.prepare(
      `SELECT last_seen_at,app_version,stationhead_ok,outbox_count,last_sequence
         FROM device_heartbeats WHERE device_id=?1`,
    ).bind("homepanel-device").first<HeartbeatRow>();
    expect(row).toEqual({
      last_seen_at: startedAt + 60_000,
      app_version: "2.14.0",
      stationhead_ok: 0,
      outbox_count: 2,
      last_sequence: 2,
    });
  });
});
