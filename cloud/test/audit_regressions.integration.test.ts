import { applyD1Migrations, env, SELF } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { runScheduler } from "../src/scheduler";
import { readUpdateManifestIdentity } from "../src/update_proxy";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & {
  TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1];
  UPDATE_BUCKET: R2Bucket;
};

const auth = (token: string): HeadersInit => ({
  Authorization: `Bearer ${token}`,
  "content-type": "application/json",
});

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
});

describe("repository audit regressions", () => {
  it("allows DEVICE_TOKEN for administrative commands and rejects unknown tokens", async () => {
    const request = (token: string) => SELF.fetch(
      "https://homepanel.test/v1/device/commands",
      {
        method: "POST",
        headers: auth(token),
        body: JSON.stringify({
          deviceId: "homepanel-device",
          command: "restart_app",
        }),
      },
    );

    expect((await request("test-device")).status).toBe(202);
    expect((await request("not-a-configured-token")).status).toBe(401);
  });

  it("reports an uninitialized data set as stale instead of healthy", async () => {
    const response = await SELF.fetch("https://homepanel.test/v1/meta", {
      headers: auth("test-device"),
    });
    expect(response.status).toBe(200);
    await expect(response.json()).resolves.toMatchObject({
      status: "stale",
      dashboardVersion: 0,
      radarVersion: 0,
    });
  });

  it("drains more than one three-job scheduler batch", async () => {
    await env.DB.prepare("DELETE FROM jobs").run();
    const statements = Array.from({ length: 7 }, (_, index) =>
      env.DB.prepare(
        "INSERT INTO jobs(name,interval_seconds,next_run_at) VALUES(?1,300,0)",
      ).bind(`audit-unknown-${index}`));
    await env.DB.batch(statements);

    await runScheduler(env);

    const row = await env.DB.prepare(
      `SELECT COUNT(*) AS total,
              SUM(CASE WHEN lease_until IS NULL AND next_run_at>0 THEN 1 ELSE 0 END) AS finished
         FROM jobs
        WHERE name LIKE 'audit-unknown-%'`,
    ).first<{ total: number; finished: number }>();
    expect(Number(row?.total)).toBe(7);
    expect(Number(row?.finished)).toBe(7);
  });

  it("changes release identity when a same-version manifest is repaired", async () => {
    const bucket = (env as TestEnv).UPDATE_BUCKET;
    const manifest = (sha256: string) => JSON.stringify({
      version: "2607101234",
      signed: false,
      files: [
        { name: "HomePanel.exe", sha256, size: 100, requireAuthenticode: false },
        { name: "HomePanelUpdater.exe", sha256: "b".repeat(64), size: 101, requireAuthenticode: false },
        { name: "WebView2Loader.dll", sha256: "c".repeat(64), size: 102, requireAuthenticode: true },
      ],
    });

    await bucket.put(
      "updates/latest/update-manifest.json",
      manifest("a".repeat(64)),
    );
    const first = await readUpdateManifestIdentity(env);

    await bucket.put(
      "updates/latest/update-manifest.json",
      manifest("d".repeat(64)),
    );
    const repaired = await readUpdateManifestIdentity(env);

    expect(repaired.version).toBe(first.version);
    expect(repaired.manifestHash).not.toBe(first.manifestHash);
  });

  it("serves admin defaults matching the native Stationhead layout", async () => {
    const response = await SELF.fetch("https://homepanel.test/admin");
    const page = await response.text();
    expect(page).toContain("https://www.stationhead.com/sakuramankai");
    expect(page).toContain("https://www.stationhead.com/buddy46");
    expect(page).toContain("width:1920,height:1280");
  });
});
