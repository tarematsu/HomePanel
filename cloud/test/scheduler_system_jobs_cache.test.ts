import { describe, expect, it, vi } from "vitest";
import type { Env } from "../src/sources";
import { ensureSystemJobs, invalidateSystemJobsCache } from "../src/scheduler";

function fakeEnv() {
  const run = vi.fn().mockResolvedValue({ meta: { changes: 0 } });
  const bind = vi.fn(() => ({ run }));
  const prepare = vi.fn(() => ({ bind }));
  const DB = { prepare } as unknown as D1Database;
  return { env: { DB } as Env, DB, prepare, run };
}

describe("system job reconciliation cache", () => {
  it("reuses a successful reconciliation for fifteen minutes", async () => {
    const { env, DB, prepare } = fakeEnv();
    invalidateSystemJobsCache(DB);

    await ensureSystemJobs(env, 1_000);
    await ensureSystemJobs(env, 1_000 + 14 * 60_000);
    expect(prepare).toHaveBeenCalledTimes(1);

    await ensureSystemJobs(env, 1_000 + 15 * 60_000);
    expect(prepare).toHaveBeenCalledTimes(2);
  });

  it("runs again after explicit invalidation", async () => {
    const { env, DB, prepare } = fakeEnv();
    await ensureSystemJobs(env, 5_000);
    invalidateSystemJobsCache(DB);
    await ensureSystemJobs(env, 5_001);
    expect(prepare).toHaveBeenCalledTimes(2);
  });
});
