import { applyD1Migrations, env, SELF } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
});

const auth = (token = "test-device"): HeadersInit => ({ Authorization: `Bearer ${token}` });

describe("HomePanel Worker integration", () => {
  it("serves health and protects device reads", async () => {
    const health = await SELF.fetch("https://homepanel.test/v1/health");
    expect(health.status).toBe(200);
    expect((await health.json<{ ok: boolean }>()).ok).toBe(true);
    expect((await SELF.fetch("https://homepanel.test/v1/meta")).status).toBe(401);
    expect((await SELF.fetch("https://homepanel.test/v1/update/file/HomePanel.exe")).status).toBe(401);
  });

  it("does not expose the retired legacy telemetry route", async () => {
    const response = await SELF.fetch("https://homepanel.test/v1/telemetry", {
      method: "POST",
      headers: { ...auth(), "content-type": "application/json" },
      body: JSON.stringify({ deviceId: "ci-device", samples: [] }),
    });
    expect(response.status).toBe(404);
    const schema = await env.DB.prepare(
      `SELECT name FROM sqlite_schema
        WHERE type='table' AND name IN ('environment_samples','environment_buckets')
        ORDER BY name`,
    ).all<{ name: string }>();
    expect(schema.results).toEqual([]);
  });

  it("isolates device commands by configured device token", async () => {
    const created = await SELF.fetch("https://homepanel.test/v1/device/commands", {
      method: "POST",
      headers: { ...auth("test-action"), "content-type": "application/json" },
      body: JSON.stringify({ deviceId: "device-b", command: "check_update" }),
    });
    expect(created.status).toBe(202);
    expect((await SELF.fetch("https://homepanel.test/v1/device/commands?deviceId=device-b", { headers: auth("token-a") })).status).toBe(401);
    const allowed = await SELF.fetch("https://homepanel.test/v1/device/commands?deviceId=device-b", { headers: auth("token-b") });
    expect(allowed.status).toBe(200);
    expect((await allowed.json<{ commands: unknown[] }>()).commands).toHaveLength(1);
  });

  it("uses optimistic locking without creating restart commands", async () => {
    const url = "https://homepanel.test/v1/device/config?deviceId=homepanel-device";
    const withoutPrecondition = await SELF.fetch(url, {
      method: "PUT",
      headers: { ...auth("test-action"), "content-type": "application/json" },
      body: JSON.stringify({ cloudPollSeconds: 60 }),
    });
    expect(withoutPrecondition.status).toBe(428);

    const saved = await SELF.fetch(url, {
      method: "PUT",
      headers: { ...auth("test-action"), "content-type": "application/json", "If-Match": '"device-config-homepanel-device-0"' },
      body: JSON.stringify({ cloudPollSeconds: 60 }),
    });
    expect(saved.status).toBe(200);
    expect(saved.headers.get("etag")).toBe('"device-config-homepanel-device-1"');

    const stale = await SELF.fetch(url, {
      method: "PUT",
      headers: { ...auth("test-action"), "content-type": "application/json", "If-Match": '"device-config-homepanel-device-0"' },
      body: JSON.stringify({ cloudPollSeconds: 30 }),
    });
    expect(stale.status).toBe(412);
    const pending = await env.DB.prepare(
      "SELECT COUNT(*) AS count FROM device_commands WHERE device_id=?1 AND completed_at IS NULL",
    ).bind("homepanel-device").first<{ count: number }>();
    expect(Number(pending?.count)).toBe(0);

    const rejected = await SELF.fetch("https://homepanel.test/v1/device/commands", {
      method: "POST",
      headers: { ...auth("test-action"), "content-type": "application/json" },
      body: JSON.stringify({ deviceId: "homepanel-device", command: "restart_app" }),
    });
    expect(rejected.status).toBe(400);
  });

  it("rejects radar tile coordinates outside the zoom range", async () => {
    const response = await SELF.fetch("https://homepanel.test/v1/radar/tile/gsi/1/999999999/999999999.png");
    expect(response.status).toBe(404);
  });

  it("serves an admin default that uses the Worker update manifest", async () => {
    const response = await SELF.fetch("https://homepanel.test/admin");
    const page = await response.text();
    expect(page).toContain('/v1/update/manifest');
    expect(page).not.toContain("releases/download/homepanel-latest/manifest.json");
  });

  it("returns a stable JSON 404", async () => {
    const response = await SELF.fetch("https://homepanel.test/missing");
    expect(response.status).toBe(404);
    await expect(response.json()).resolves.toEqual({ error: "not found" });
  });
});
