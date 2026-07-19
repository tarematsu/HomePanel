import { describe, expect, it, vi } from "vitest";
import type { Env } from "../src/sources";
import { runSchedulerFromDeviceSync } from "../src/worker_core";

function testEnv(): Env {
  return { DB: {} as D1Database } as Env;
}

describe("device sync scheduler gate", () => {
  it("coalesces concurrent runs and applies a four-minute success cooldown", async () => {
    const env = testEnv();
    let finish!: () => void;
    const runner = vi.fn(() => new Promise<void>(resolve => { finish = resolve; }));

    const first = runSchedulerFromDeviceSync(env, 10_000, runner);
    const concurrent = runSchedulerFromDeviceSync(env, 10_001, runner);
    expect(first).not.toBeNull();
    expect(concurrent).toBe(first);
    expect(runner).toHaveBeenCalledTimes(1);

    finish();
    await first;
    expect(runSchedulerFromDeviceSync(env, 249_999, runner)).toBeNull();

    const afterCooldown = runSchedulerFromDeviceSync(env, 250_000, runner);
    expect(afterCooldown).not.toBeNull();
    expect(runner).toHaveBeenCalledTimes(2);
    finish();
    await afterCooldown;
  });

  it("allows the next sync to retry immediately after a failed run", async () => {
    const env = testEnv();
    const failure = new Error("scheduler failed");
    const runner = vi.fn()
      .mockRejectedValueOnce(failure)
      .mockResolvedValueOnce(undefined);

    await expect(runSchedulerFromDeviceSync(env, 20_000, runner)).rejects.toBe(failure);
    await expect(runSchedulerFromDeviceSync(env, 20_001, runner)).resolves.toBeUndefined();
    expect(runner).toHaveBeenCalledTimes(2);
  });
});
