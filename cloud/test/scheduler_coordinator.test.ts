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
type RuntimeJob = {
  name: string;
  intervalSeconds: number;
  nextRunAt: number;
  lastSuccessAt: number | null;
  consecutiveFailures: number;
  lastError: string | null;
};
type RuntimeEnvelope = { version: number; jobs: RuntimeJob[] };

const RUNTIME_STORAGE_KEY = "scheduler-runtime-v2";
let objectSequence = 0;

function coordinatorStub(): DurableObjectStub {
  const namespace = (env as TestEnv).SCHEDULER_COORDINATOR;
  const id = namespace.idFromName(`scheduler-test-${objectSequence++}`);
  return namespace.get(id);
}

async function alarmTime(stub: DurableObjectStub): Promise<number | null> {
  return runInDurableObject(stub, async (_instance, state) => state.storage.getAlarm());
}

async function runtime(stub: DurableObjectStub): Promise<RuntimeEnvelope | undefined> {
  return runInDurableObject(stub, async (_instance, state) =>
    state.storage.get<RuntimeEnvelope>(RUNTIME_STORAGE_KEY));
}

async function runAlarm(stub: DurableObjectStub): Promise<void> {
  await runInDurableObject(stub, async instance => {
    await (instance as AlarmInstance).alarm();
  });
}

async function insertFailingJob(name: string): Promise<void> {
  await env.DB.prepare(
    `INSERT INTO jobs(
       name,interval_seconds,next_run_at,lease_until,last_success_at,last_error,consecutive_failures
     ) VALUES(?1,900,0,NULL,NULL,NULL,0)`,
  ).bind(name).run();
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

  it("imports video liveness into the DO runtime once", async () => {
    await env.DB.prepare("DELETE FROM jobs WHERE name=?1").bind(LIVENESS_JOB_NAME).run();
    invalidateSystemJobsCache(env.DB);
    const stub = coordinatorStub();

    const response = await stub.fetch("https://scheduler.internal/ensure", { method: "POST" });
    const stored = await runtime(stub);
    const liveness = stored?.jobs.find(job => job.name === LIVENESS_JOB_NAME);

    expect(response.status).toBe(202);
    expect(liveness).toMatchObject({
      name: LIVENESS_JOB_NAME,
      intervalSeconds: LIVENESS_INTERVAL_SECONDS,
      consecutiveFailures: 0,
      lastError: null,
    });
    expect(Number(liveness?.nextRunAt)).toBeGreaterThan(0);
  });

  it("schedules an alarm for the earliest runtime job", async () => {
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

  it("advances successful work only in DO storage and does not write D1 job state", async () => {
    const now = Math.floor(Date.now() / 1000);
    await env.DB.prepare("UPDATE jobs SET next_run_at=?1, lease_until=NULL")
      .bind(now + 3600)
      .run();
    await env.DB.prepare("UPDATE jobs SET next_run_at=0 WHERE name='cleanup'").run();
    const before = await env.DB.prepare(
      "SELECT next_run_at,lease_until,last_success_at FROM jobs WHERE name='cleanup'",
    ).first<{ next_run_at: number; lease_until: number | null; last_success_at: number | null }>();
    const stub = coordinatorStub();
    await stub.fetch("https://scheduler.internal/ensure", { method: "POST" });

    await runAlarm(stub);

    const after = await env.DB.prepare(
      "SELECT next_run_at,lease_until,last_success_at FROM jobs WHERE name='cleanup'",
    ).first<{ next_run_at: number; lease_until: number | null; last_success_at: number | null }>();
    expect(after).toEqual(before);

    const stored = await runtime(stub);
    const cleanup = stored?.jobs.find(job => job.name === "cleanup");
    expect(Number(cleanup?.nextRunAt)).toBeGreaterThan(now);
    expect(Number(cleanup?.lastSuccessAt)).toBeGreaterThanOrEqual(now);
    expect(cleanup?.consecutiveFailures).toBe(0);

    const events = await env.DB.prepare(
      "SELECT COUNT(*) AS count FROM job_events WHERE job_name='cleanup'",
    ).first<{ count: number }>();
    expect(Number(events?.count)).toBe(0);
    expect(Number(await alarmTime(stub))).toBeGreaterThan(Date.now());
  });

  it("advances up to three co-due jobs in one alarm", async () => {
    const now = Math.floor(Date.now() / 1000);
    await env.DB.prepare("UPDATE jobs SET next_run_at=?1, lease_until=NULL")
      .bind(now + 3600)
      .run();
    await env.DB.prepare(
      "UPDATE jobs SET next_run_at=0 WHERE name IN ('cleanup','update_check',?1)",
    ).bind(LIVENESS_JOB_NAME).run();
    const stub = coordinatorStub();
    await stub.fetch("https://scheduler.internal/ensure", { method: "POST" });

    await runAlarm(stub);

    const stored = await runtime(stub);
    const selected = stored?.jobs.filter(job => ["cleanup", "update_check", LIVENESS_JOB_NAME].includes(job.name)) ?? [];
    expect(selected).toHaveLength(3);
    expect(selected.every(job => job.nextRunAt > now)).toBe(true);
  });

  it("records only the first failure until the job recovers", async () => {
    const now = Math.floor(Date.now() / 1000);
    await env.DB.prepare("UPDATE jobs SET next_run_at=?1, lease_until=NULL").bind(now + 3600).run();
    await insertFailingJob("unsupported_source");
    const stub = coordinatorStub();
    await stub.fetch("https://scheduler.internal/ensure", { method: "POST" });

    await runAlarm(stub);
    await stub.fetch("https://scheduler.internal/wake", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ names: ["unsupported_source"] }),
    });
    await runAlarm(stub);

    const events = await env.DB.prepare(
      "SELECT event,COUNT(*) AS count FROM job_events WHERE job_name=?1 GROUP BY event",
    ).bind("unsupported_source").all<{ event: string; count: number }>();
    expect(events.results).toEqual([{ event: "failed", count: 1 }]);
    const stored = await runtime(stub);
    expect(stored?.jobs.find(job => job.name === "unsupported_source")?.consecutiveFailures).toBe(2);
  });

  it("advances runtime even when transition history cannot be written", async () => {
    const now = Math.floor(Date.now() / 1000);
    await env.DB.prepare("UPDATE jobs SET next_run_at=?1, lease_until=NULL").bind(now + 3600).run();
    await insertFailingJob("history_failure_source");
    await env.DB.prepare(
      `CREATE TRIGGER reject_job_events
       BEFORE INSERT ON job_events
       BEGIN
         SELECT RAISE(FAIL,'job event storage unavailable');
       END`,
    ).run();
    const stub = coordinatorStub();
    await stub.fetch("https://scheduler.internal/ensure", { method: "POST" });

    await runAlarm(stub);

    const stored = await runtime(stub);
    const failed = stored?.jobs.find(job => job.name === "history_failure_source");
    expect(failed?.consecutiveFailures).toBe(1);
    expect(Number(failed?.nextRunAt)).toBeGreaterThan(now);
    const events = await env.DB.prepare(
      "SELECT COUNT(*) AS count FROM job_events WHERE job_name=?1",
    ).bind("history_failure_source").first<{ count: number }>();
    expect(Number(events?.count)).toBe(0);
    await env.DB.prepare("DROP TRIGGER reject_job_events").run();
  });

  it("refreshes only the requested runtime job without touching D1", async () => {
    const now = Math.floor(Date.now() / 1000);
    await env.DB.prepare("UPDATE jobs SET next_run_at=?1, lease_until=NULL")
      .bind(now + 3600)
      .run();
    const stub = coordinatorStub();
    await stub.fetch("https://scheduler.internal/ensure", { method: "POST" });
    const futureAlarm = await alarmTime(stub);
    const before = await env.DB.prepare(
      "SELECT next_run_at FROM jobs WHERE name='weather'",
    ).first<{ next_run_at: number }>();

    const response = await stub.fetch("https://scheduler.internal/wake", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ names: ["weather"] }),
    });
    const immediateAlarm = await alarmTime(stub);
    const stored = await runtime(stub);
    const after = await env.DB.prepare(
      "SELECT next_run_at FROM jobs WHERE name='weather'",
    ).first<{ next_run_at: number }>();

    expect(response.status).toBe(202);
    await expect(response.clone().json()).resolves.toMatchObject({ scheduled: true, changed: 1 });
    expect(futureAlarm).not.toBeNull();
    expect(immediateAlarm).not.toBeNull();
    expect(Number(immediateAlarm)).toBeLessThan(Number(futureAlarm));
    expect(Number(immediateAlarm)).toBeLessThanOrEqual(Date.now() + 5_000);
    expect(stored?.jobs.find(job => job.name === "weather")?.nextRunAt)
      .toBeLessThanOrEqual(Math.floor(Date.now() / 1000));
    expect(after).toEqual(before);
    expect(stored?.jobs.filter(job => job.nextRunAt <= now).map(job => job.name)).toEqual(["weather"]);
  });
});
