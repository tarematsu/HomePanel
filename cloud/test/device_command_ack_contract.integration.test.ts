import { applyD1Migrations, env, SELF } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & { TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1] };

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
});

const authorization = (token: string): HeadersInit => ({ Authorization: `Bearer ${token}` });

async function createCommand(): Promise<number> {
  const response = await SELF.fetch("https://homepanel.test/v1/device/commands", {
    method: "POST",
    headers: { ...authorization("test-action"), "content-type": "application/json" },
    body: JSON.stringify({ deviceId: "ci-device", command: "check_update" }),
  });
  expect(response.status).toBe(202);
  return Number((await response.json<{ id: number }>()).id);
}

async function postAck(id: unknown, values: Record<string, unknown>): Promise<Response> {
  return SELF.fetch("https://homepanel.test/v1/device/commands/ack?deviceId=ci-device", {
    method: "POST",
    headers: { ...authorization("test-device"), "content-type": "application/json" },
    body: JSON.stringify({ id, ...values }),
  });
}

describe("device command acknowledgement contract", () => {
  it("keeps a command pending until success is a boolean", async () => {
    const id = await createCommand();
    for (const values of [{}, { success: "true" }, { success: 1 }, { success: null }]) {
      expect((await postAck(id, values)).status).toBe(400);
    }
    const row = await env.DB.prepare(
      "SELECT completed_at,success FROM device_commands WHERE id=?1",
    ).bind(id).first<{ completed_at: number | null; success: number | null }>();
    expect(row).toEqual({ completed_at: null, success: null });
  });

  it("rejects coerced or fractional command ids", async () => {
    const id = await createCommand();
    for (const invalidId of [String(id), id + 0.5, true, null]) {
      expect((await postAck(invalidId, { success: true })).status).toBe(400);
    }
    const row = await env.DB.prepare(
      "SELECT completed_at,success FROM device_commands WHERE id=?1",
    ).bind(id).first<{ completed_at: number | null; success: number | null }>();
    expect(row).toEqual({ completed_at: null, success: null });
  });

  it("records an explicit failed acknowledgement", async () => {
    const id = await createCommand();
    expect((await postAck(id, { success: false, result: 7 })).status).toBe(400);
    const response = await postAck(id, { success: false, result: "not completed" });
    expect(response.status).toBe(200);
    await expect(response.json()).resolves.toEqual({ acknowledged: true });
    const row = await env.DB.prepare(
      "SELECT completed_at,success,result FROM device_commands WHERE id=?1",
    ).bind(id).first<{ completed_at: number | null; success: number | null; result: string | null }>();
    expect(Number(row?.completed_at ?? 0)).toBeGreaterThan(0);
    expect(row?.success).toBe(0);
    expect(row?.result).toBe("not completed");
  });
});
