import { readFileSync } from "node:fs";
import { describe, expect, it, vi } from "vitest";
import { getDeviceSync } from "../src/device_sync";
import type { Env } from "../src/sources";

describe("device sync unchanged fast path", () => {
  it("passes parsed exchange versions directly without constructing an internal request", () => {
    const source = readFileSync(new URL("../src/device_exchange.ts", import.meta.url), "utf8");
    expect(source).toContain("buildDeviceSyncPayloadForDevice(env, deviceId, versions)");
    expect(source).not.toContain("new Request(");
    expect(source).not.toContain("new URL(");
  });

  it("uses one snapshot statement and does not fetch state payload rows when versions match", async () => {
    const statements: string[] = [];
    const all = vi.fn().mockResolvedValue({
      results: [{
        dashboard_version: 27,
        radar_version: 8,
        switchbot_version: 5,
        stationhead_version: 6,
        stationhead_health_version: 10,
        config_version: 9,
        config_updated_at: 123,
        config_payload: "{}",
        pending: 0,
      }],
    });
    const prepare = vi.fn((sql: string) => {
      statements.push(sql);
      return {
        bind: vi.fn(() => ({ all })),
      };
    });
    const env = {
      DB: { prepare } as unknown as D1Database,
    } as Env;
    const request = new Request(
      "https://homepanel.test/v1/device/sync?deviceId=homepanel-device" +
      "&dashboardVersion=27&radarVersion=8&switchbotVersion=5" +
      "&stationheadVersion=6&stationheadHealthVersion=10&configVersion=9",
    );

    const response = await getDeviceSync(request, env);

    expect(response.status).toBe(200);
    await expect(response.json()).resolves.toEqual({
      workerVersion: "2.12.0",
      versions: {
        dashboard: 27,
        radar: 8,
        switchbot: 5,
        stationhead: 6,
        stationheadHealth: 10,
        config: 9,
      },
      commands: [],
    });
    expect(prepare).toHaveBeenCalledTimes(1);
    expect(all).toHaveBeenCalledTimes(1);
    expect(statements[0]).toContain("SUM(CASE");
    expect(statements[0]).toContain("device_configs");
    expect(statements[0]).toContain("device_commands");
    expect(statements.some(sql => sql.includes("SELECT source,version,payload"))).toBe(false);
  });
});
