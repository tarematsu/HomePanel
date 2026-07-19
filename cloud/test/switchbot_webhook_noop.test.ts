import { describe, expect, it, vi } from "vitest";
import { switchBotWebhookStateStillCurrent } from "../src/switchbot";
import type { StateRow } from "../src/snapshot";
import type { SwitchBotEnv } from "../src/switchbot_types";

function stateRow(overrides: Partial<StateRow> = {}): StateRow {
  return {
    source: "switchbot",
    version: 7,
    payload: "{\"presence\":\"home\"}",
    observed_at: 1_800_000_000_000,
    fetched_at: 1_800_000_000_000,
    last_success_at: 1_800_000_000_000,
    status: "ok",
    error: null,
    content_hash: "hash",
    ...overrides,
  };
}

function fakeEnv(result: { present: number } | null) {
  const first = vi.fn().mockResolvedValue(result);
  const bind = vi.fn(() => ({ first }));
  const prepare = vi.fn(() => ({ bind }));
  const env = { DB: { prepare } as unknown as D1Database } as SwitchBotEnv;
  return { env, prepare, bind, first };
}

describe("SwitchBot exact webhook no-op", () => {
  it("accepts a fresh identical row after a one-row version check", async () => {
    const now = 1_800_000_100_000;
    const row = stateRow();
    const { env, prepare, bind, first } = fakeEnv({ present: 1 });

    await expect(switchBotWebhookStateStillCurrent(env, row, row.payload, now)).resolves.toBe(true);
    expect(prepare).toHaveBeenCalledTimes(1);
    expect(String((prepare.mock.calls as unknown[][])[0]?.[0] ?? ""))
      .toContain("source='switchbot' AND version=?1");
    expect(bind).toHaveBeenCalledWith(row.version, now - 15 * 60_000);
    expect(first).toHaveBeenCalledTimes(1);
  });

  it("does not query D1 when the local payload already differs", async () => {
    const { env, prepare } = fakeEnv({ present: 1 });
    const row = stateRow();

    await expect(switchBotWebhookStateStillCurrent(
      env,
      row,
      "{\"presence\":\"away\"}",
      row.fetched_at + 1,
    )).resolves.toBe(false);
    expect(prepare).not.toHaveBeenCalled();
  });

  it("falls back when the row was deleted or replaced", async () => {
    const row = stateRow();
    const { env, first } = fakeEnv(null);

    await expect(switchBotWebhookStateStillCurrent(
      env,
      row,
      row.payload,
      row.fetched_at + 1,
    )).resolves.toBe(false);
    expect(first).toHaveBeenCalledTimes(1);
  });

  it("falls back after the state heartbeat expires", async () => {
    const row = stateRow();
    const { env, prepare } = fakeEnv({ present: 1 });

    await expect(switchBotWebhookStateStillCurrent(
      env,
      row,
      row.payload,
      row.fetched_at + 15 * 60_000,
    )).resolves.toBe(false);
    expect(prepare).not.toHaveBeenCalled();
  });
});
