import { applyD1Migrations, env, SELF } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { runUpdateCheck } from "../src/update_check";
import type { Env } from "../src/sources";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

const MANIFEST_KEY = "updates/latest/update-manifest.json";

function manifest(version: string): string {
  return JSON.stringify({
    version,
    signed: false,
    files: [
      { name: "HomePanel.exe", sha256: "a".repeat(64), size: 1 },
      { name: "HomePanelUpdater.exe", sha256: "b".repeat(64), size: 1 },
      { name: "WebView2Loader.dll", sha256: "c".repeat(64), size: 1 },
    ],
  });
}

function updateEnv(): Env {
  // Pin the device population for the test: no static token map, one primary
  // device, plus whatever heartbeats the test inserts.
  return {
    ...env,
    UPDATE_BUCKET_PREFIX: "updates",
    HOMEPANEL_DEVICE_TOKENS: "",
    HOMEPANEL_PRIMARY_DEVICE_ID: "tablet-1",
  } as Env;
}

async function pendingCommands(): Promise<Array<{ device_id: string; command: string; payload: string | null }>> {
  const rows = await env.DB.prepare(
    "SELECT device_id, command, payload FROM device_commands WHERE completed_at IS NULL ORDER BY id",
  ).all<{ device_id: string; command: string; payload: string | null }>();
  return rows.results ?? [];
}

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await applyD1Migrations(testEnv.DB, testEnv.TEST_MIGRATIONS);
  await testEnv.DB.batch([
    testEnv.DB.prepare("DELETE FROM device_commands"),
    testEnv.DB.prepare("DELETE FROM device_heartbeats"),
    testEnv.DB.prepare("DELETE FROM current_state"),
  ]);
});

describe("cloud-driven update check", () => {
  it("seeds the update_check job via migrations", async () => {
    const job = await env.DB.prepare("SELECT interval_seconds FROM jobs WHERE name='update_check'")
      .first<{ interval_seconds: number }>();
    expect(job?.interval_seconds).toBe(300);
  });

  it("records a baseline first, then queues one command per device per release", async () => {
    const scoped = updateEnv();
    await env.UPDATE_BUCKET!.put(MANIFEST_KEY, manifest("2607100001"));
    await env.DB.prepare(
      "INSERT INTO device_heartbeats(device_id, last_seen_at, stationhead_ok, outbox_count, last_sequence) VALUES('tablet-2', ?1, 1, 0, 0)",
    ).bind(Date.now()).run();

    // First observation is a baseline: no commands yet.
    await runUpdateCheck(scoped);
    expect(await pendingCommands()).toHaveLength(0);

    // Re-running with the same version and manifest stays quiet.
    await runUpdateCheck(scoped);
    expect(await pendingCommands()).toHaveLength(0);

    // A new release queues exactly one check_update per known device.
    await env.UPDATE_BUCKET!.put(MANIFEST_KEY, manifest("2607100002"));
    await runUpdateCheck(scoped);
    let commands = await pendingCommands();
    expect(commands.map(command => command.device_id).sort()).toEqual(["tablet-1", "tablet-2"]);
    expect(commands.every(command => command.command === "check_update")).toBe(true);
    expect(commands.every(command => command.payload?.includes("2607100002"))).toBe(true);

    // Idempotent while the release identity stays put, even across repeated runs.
    await runUpdateCheck(scoped);
    expect(await pendingCommands()).toHaveLength(2);

    // The next release queues a second round.
    await env.UPDATE_BUCKET!.put(MANIFEST_KEY, manifest("2607100003"));
    await runUpdateCheck(scoped);
    commands = await pendingCommands();
    expect(commands).toHaveLength(4);
  });

  it("accepts unauthenticated pings and throttles repeats", async () => {
    await env.UPDATE_BUCKET.put(MANIFEST_KEY, manifest("2607100001"));
    const first = await SELF.fetch("https://example.test/v1/update/ping", { method: "POST" });
    expect(first.status).toBe(202);
    const firstBody = await first.json() as { queued: boolean };
    // A second ping within the throttle window is accepted but not re-queued.
    const second = await SELF.fetch("https://example.test/v1/update/ping", { method: "POST" });
    expect(second.status).toBe(202);
    const secondBody = await second.json() as { queued: boolean };
    expect(firstBody.queued || secondBody.queued).toBe(true);
    expect(firstBody.queued && secondBody.queued).toBe(false);
    expect((await SELF.fetch("https://example.test/v1/update/ping")).status).not.toBe(202);
  });

  it("rejects an invalid manifest without recording a new baseline", async () => {
    const scoped = updateEnv();
    await env.UPDATE_BUCKET!.put(MANIFEST_KEY, manifest("2607100001"));
    await runUpdateCheck(scoped);
    await env.UPDATE_BUCKET!.put(MANIFEST_KEY, "{\"version\":\"not-a-version\",\"files\":[]}");
    await expect(runUpdateCheck(scoped)).rejects.toThrow();
    await env.UPDATE_BUCKET!.put(MANIFEST_KEY, manifest("2607100002"));
    await runUpdateCheck(scoped);
    expect(await pendingCommands()).toHaveLength(1);
  });
});
