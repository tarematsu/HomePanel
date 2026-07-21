import {
  applyD1Migrations,
  env,
  runInDurableObject,
} from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import {
  LIVENESS_INTERVAL_SECONDS,
  LIVENESS_JOB_NAME,
} from "../../video/src/liveness-schedule.js";
import {
  ensureSystemJobs,
  invalidateSystemJobsCache,
} from "../src/scheduler";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & {
  TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1];
  SCHEDULER_COORDINATOR: DurableObjectNamespace;
};

type AlarmInstance = { alarm(): Promise<void> };

let objectSequence = 0;

function coordinatorStub(): DurableObjectStub {
  const namespace = (env as TestEnv).SCHEDULER_COORDINATOR;
  const id = namespace.idFromName(`scheduler-test-${objectSequence++}`);
  return namespace.get(id);
}

async function alarmTime(stub: DurableObjectStub): Promise<number | null> {
  return runInDurableObject(stub, async (_instance, state) => state.storage.getAlarm());
}

async function runAlarm(stub: DurableObjectStub): Promise<void> {
  // Invoke the handler directly so the test verifies scheduler behavior rather
  // than depending on the test runtime's wall-clock alarm dispatch semantics.
  await runInDurableObject(stub, async instance => {
    await (instance as AlarmInstance).alarm();
  });
}

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
  invalidateSystemJobsCache(testEnv.DB);
  await ensureSystemJobs(testEnv);
});

describe("SchedulerCoordinator Durable Object", () => {
  it("accepts only internal POST signals", async () => {
    const stub = coordinatorStub();

    const response = await stub.fetch("https://scheduler.internal/ensure");

    expect(response.status).toBe(405);
    expect(response.headers.get("allow")).toBe("POST");
    expect(await alarmTime(stub)).toBeNull();
  });

  it("registers video liveness in the shared alarm scheduler", async () => {
    await env.DB.prepare("DELETE FROM jobs WHERE name=?1").bind(LIVENESS_JOB_NAME).run();
    invalidateSystemJobsCache(env.DB);
    const stub = coordinatorStub();

    const response = await stub.fetch("https://scheduler.internal/ensure", { method: "POST" });
    const row = await env.DB.prepare(
      "SELECT name,interval_seconds,next_run_at FROM jobs WHERE name=?1",
    ).bind(LIVENESS_JOB_NAME).first<{
      name: string;
      interval_seconds: number;
      next_run_at: number;
    }>();

    expect(response.status).toBe(202);
    expect(row).toEqual({
      name: LIVENESS_JOB_NAME,
      interval_seconds: LIVENESS_INTERVAL_SECONDS,
      next_run_at: 0,
    });
  });

  it("schedules an alarm for the earliest due job", async () => {
    const now = Math.floor(Date.now() / 1000);
    await env.DB.prepare("UPDATE jobs SET next_run_at=?1, lease_until=NULL")
      .bind(now + 3600)
      .run();
    await env.DB.prepare("UPDATE jobs SET next_run_at=0 WHERE name='cleanup'").run();
    const stub = coordinatorStub();

    const response = await stub.fetch("https://scheduler.internal/ensure", { method: "POST" });

    expect(response.status).toBe(202);
    await expect(response.json()).resolves.toMatchObject({ scheduled: true });
    const scheduledAt = await alarmTime(stub);
    expect(scheduledAt).not.toBeNull();
    expect(Number(scheduledAt)).toBeGreaterThan(Date.now());
    expect(Number(scheduledAt)).toBeLessThanOrEqual(Date.now() + 5_000);
  });

  it("runs one due job per alarm and schedules remaining work", async () => {
    const now = Math.floor(Date.now() / 1000);
    await env.DB.prepare("UPDATE jobs SET next_run_at=?1, lease_until=NULL")
      .bind(now + 3600)
      .run();
    await env.DB.prepare("UPDATE jobs SET next_run_at=0 WHERE name IN ('cleanup','weather')").run();
    const stub = coordinatorStub();
    await stub.fetch("https://scheduler.internal/ensure", { method: "POST" });

    await runAlarm(stub);

    let rows = await env.DB.prepare(
      "SELECT name,next_run_at,lease_until FROM jobs WHERE name IN ('cleanup','weather') ORDER BY name",
    ).all<{ name: string; next_run_at: number; lease_until: number | null }>();
    expect(rows.results).toHaveLength(2);
    expect(rows.results?.filter(row => Number(row.next_run_at) > now)).toHaveLength(1);
    expect(rows.results?.filter(row => Number(row.next_run_at) === 0)).toHaveLength(1);
    for (const row of rows.results ?? []) expect(row.lease_until).toBeNull();

    const remainingAlarm = await alarmTime(stub);
    expect(remainingAlarm).not.toBeNull();
    expect(Number(remainingAlarm)).toBeGreaterThan(Date.now());
    expect(Number(remainingAlarm)).toBeLessThanOrEqual(Date.now() + 5_000);

    await runAlarm(stub);

    rows = await env.DB.prepare(
      "SELECT name,next_run_at,lease_until FROM jobs WHERE name IN ('cleanup','weather') ORDER BY name",
    ).all<{ name: string; next_run_at: number; lease_until: number | null }>();
    expect(rows.results).toHaveLength(2);
    for (const row of rows.results ?? []) {
      expect(row.lease_until).toBeNull();
      expect(Number(row.next_run_at)).toBeGreaterThan(now);
    }

    const nextAlarm = await alarmTime(stub);
    expect(nextAlarm).not.toBeNull();
    expect(Number(nextAlarm)).toBeGreaterThan(Date.now());
  });

  it("moves a future alarm forward when refresh work wakes the coordinator", async () => {
    const now = Math.floor(Date.now() / 1000);
    await env.DB.prepare("UPDATE jobs SET next_run_at=?1, lease_until=NULL")
      .bind(now + 3600)
      .run();
    const stub = coordinatorStub();
    await stub.fetch("https://scheduler.internal/ensure", { method: "POST" });
    const futureAlarm = await alarmTime(stub);

    const response = await stub.fetch("https://scheduler.internal/wake", { method: "POST" });
    const immediateAlarm = await alarmTime(stub);

    expect(response.status).toBe(202);
    expect(futureAlarm).not.toBeNull();
    expect(immediateAlarm).not.toBeNull();
    expect(Number(immediateAlarm)).toBeLessThan(Number(futureAlarm));
    expect(Number(immediateAlarm)).toBeLessThanOrEqual(Date.now() + 5_000);
  });
});
