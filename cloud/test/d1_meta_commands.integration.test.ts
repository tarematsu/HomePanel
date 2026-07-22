import { applyD1Migrations, env, SELF } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
});

const auth = (token: string): HeadersInit => ({ Authorization: `Bearer ${token}` });

async function createCommand(): Promise<Response> {
  return SELF.fetch("https://homepanel.test/v1/device/commands", {
    method: "POST",
    headers: { ...auth("test-action"), "content-type": "application/json" },
    body: JSON.stringify({
      deviceId: "device-b",
      command: "check_update",
      payload: { channel: "stable" },
    }),
  });
}

async function insertState(
  source: string,
  version: number,
  status: "ok" | "stale" | "error",
  payload: unknown = {},
): Promise<void> {
  const now = Date.now();
  await env.DB.prepare(
    `INSERT INTO current_state(
       source,version,payload,observed_at,fetched_at,last_success_at,status,error,content_hash
     ) VALUES(?1,?2,?3,?4,?4,?4,?5,?6,?7)`,
  ).bind(
    source,
    version,
    JSON.stringify(payload),
    now,
    status,
    status === "ok" ? null : `${source} test status`,
    `${source}-hash`,
  ).run();
}

function jstDayKey(offsetDays: number): string {
  const date = new Date(Date.now() + 9 * 60 * 60 * 1000 + offsetDays * 86_400_000);
  return `${date.getUTCFullYear()}-${String(date.getUTCMonth() + 1).padStart(2, "0")}-${String(date.getUTCDate()).padStart(2, "0")}`;
}

describe("D1 meta and command optimizations", () => {
  it("derives meta versions and status directly from cloud source rows", async () => {
    await insertState("weather", 2, "ok");
    await insertState("news", 3, "stale");
    await insertState("octopus", 4, "ok");
    await insertState("switchbot", 5, "ok");
    await insertState("stationhead", 6, "ok");
    await insertState("environment", 7, "ok");
    await insertState("radar", 7, "ok");

    const response = await SELF.fetch("https://homepanel.test/v1/meta", { headers: auth("test-device") });
    expect(response.status).toBe(200);
    await expect(response.json()).resolves.toMatchObject({
      version: 34,
      dashboardVersion: 27,
      radarVersion: 7,
      status: "stale",
      workerVersion: "2.12.0",
    });
  });

  it("omits today and yesterday from the Octopus graph payload", async () => {
    await insertState("octopus", 4, "ok", {
      history: [
        { date: jstDayKey(-3), value: 3.1 },
        { date: jstDayKey(-2), value: 2.2 },
        { date: jstDayKey(-1), value: 1.1 },
        { date: jstDayKey(0), value: 0.4 },
      ],
      lastMonth: { usage: 100 },
      thisMonth: { projectedUsage: 90 },
    });
    for (const source of ["weather", "news", "switchbot", "stationhead", "environment"] as const) {
      await insertState(source, 1, "ok");
    }
    await insertState("radar", 1, "ok");

    const response = await SELF.fetch(
      "https://homepanel.test/v1/device/sync?deviceId=homepanel-device&dashboardVersion=-1&radarVersion=-1&switchbotVersion=-1&stationheadVersion=-1&configVersion=-1",
      { headers: auth("test-device") },
    );
    expect(response.status).toBe(200);
    const body = await response.json<{ dashboard: string }>();
    const dashboard = JSON.parse(body.dashboard) as { octopus: { history: Array<{ date: string }> } };
    expect(dashboard.octopus.history.map(item => item.date)).toEqual([
      jstDayKey(-3),
      jstDayKey(-2),
    ]);
  });

  it("preserves sync versions while returning only payloads with stale client versions", async () => {
    const stateVersions = {
      weather: 2,
      news: 3,
      octopus: 4,
      switchbot: 5,
      stationhead: 6,
      environment: 7,
      radar: 8,
      stationhead_health: 10,
    } as const;
    for (const [source, version] of Object.entries(stateVersions)) {
      const payload = source === "stationhead_health" ? { marker: source, healthy: true } : { marker: source };
      await insertState(source, version, "ok", payload);
    }
    const configUpdatedAt = Date.now();
    await env.DB.prepare(
      "INSERT INTO device_configs(device_id,version,payload,updated_at) VALUES(?1,?2,?3,?4)",
    ).bind("homepanel-device", 9, JSON.stringify({ cloudPollSeconds: 900 }), configUpdatedAt).run();

    const baseUrl = "https://homepanel.test/v1/device/sync?deviceId=homepanel-device&dashboardVersion=27&radarVersion=8&switchbotVersion=5&stationheadVersion=6&stationheadHealthVersion=10&configVersion=9";
    const matching = await SELF.fetch(baseUrl, { headers: auth("test-device") });
    expect(matching.status).toBe(200);
    await expect(matching.json()).resolves.toEqual({
      workerVersion: "2.12.0",
      versions: { dashboard: 27, radar: 8, switchbot: 5, stationhead: 6, stationheadHealth: 10, config: 9 },
      commands: [],
    });

    const stale = await SELF.fetch(
      baseUrl.replace("radarVersion=8", "radarVersion=7")
        .replace("switchbotVersion=5", "switchbotVersion=4")
        .replace("stationheadHealthVersion=10", "stationheadHealthVersion=9")
        .replace("configVersion=9", "configVersion=8"),
      { headers: auth("test-device") },
    );
    expect(stale.status).toBe(200);
    await expect(stale.json()).resolves.toEqual({
      workerVersion: "2.12.0",
      versions: { dashboard: 27, radar: 8, switchbot: 5, stationhead: 6, stationheadHealth: 10, config: 9 },
      commands: [],
      radar: JSON.stringify({ marker: "radar" }),
      switchbot: JSON.stringify({ marker: "switchbot" }),
      stationheadHealth: JSON.stringify({ marker: "stationhead_health", healthy: true }),
      deviceConfig: JSON.stringify({
        deviceId: "homepanel-device",
        version: 9,
        updatedAt: configUpdatedAt,
        config: { cloudPollSeconds: 900 },
      }),
    });
  });

  it("falls back stationhead_health payload to unhealthy when its status is not ok", async () => {
    for (const source of ["weather", "news", "octopus", "switchbot", "stationhead", "environment"] as const) {
      await insertState(source, 1, "ok");
    }
    await insertState("radar", 1, "ok");
    await insertState("stationhead_health", 3, "error", { healthy: true, reason: "stale reading" });

    const response = await SELF.fetch(
      "https://homepanel.test/v1/device/sync?deviceId=homepanel-device&dashboardVersion=-1&radarVersion=-1&switchbotVersion=-1&stationheadVersion=-1&stationheadHealthVersion=-1&configVersion=-1",
      { headers: auth("test-device") },
    );
    expect(response.status).toBe(200);
    const body = await response.json<{ stationheadHealth: string }>();
    expect(JSON.parse(body.stationheadHealth)).toEqual({
      healthy: false,
      monitorStatus: "error",
      reason: "stationhead_health test status",
    });
  });

  it("sets SwitchBot to fifteen minutes and Octopus to six hours", async () => {
    const rows = await env.DB.prepare(
      "SELECT name, interval_seconds FROM jobs WHERE name IN ('switchbot','octopus') ORDER BY name",
    ).all<{ name: string; interval_seconds: number }>();
    expect(rows.results).toEqual([
      { name: "octopus", interval_seconds: 21_600 },
      { name: "switchbot", interval_seconds: 900 },
    ]);
  });

  it("claims a pending command only once across concurrent device polls", async () => {
    expect((await createCommand()).status).toBe(202);
    const url = "https://homepanel.test/v1/device/commands?deviceId=device-b";
    const [first, second] = await Promise.all([
      SELF.fetch(url, { headers: auth("token-b") }),
      SELF.fetch(url, { headers: auth("token-b") }),
    ]);
    const payloads = await Promise.all([
      first.json<{ commands: Array<{ id: number }> }>(),
      second.json<{ commands: Array<{ id: number }> }>(),
    ]);
    expect(payloads[0].commands.length + payloads[1].commands.length).toBe(1);

    const immediateRetry = await SELF.fetch(url, { headers: auth("token-b") });
    expect((await immediateRetry.json<{ commands: unknown[] }>()).commands).toHaveLength(0);
  });

  it("prevents duplicate rows when identical commands are created concurrently", async () => {
    const [first, second] = await Promise.all([createCommand(), createCommand()]);
    const results = await Promise.all([
      first.json<{ id: number; deduplicated: boolean }>(),
      second.json<{ id: number; deduplicated: boolean }>(),
    ]);
    expect(results[0].id).toBe(results[1].id);
    expect(results.map(result => result.deduplicated).sort()).toEqual([false, true]);

    const count = await env.DB.prepare(
      `SELECT COUNT(*) AS count
         FROM device_commands
        WHERE device_id='device-b'
          AND command='check_update'
          AND completed_at IS NULL`,
    ).first<{ count: number }>();
    expect(Number(count?.count)).toBe(1);
  });
});
