import {
  applyD1Migrations,
  env,
  runDurableObjectAlarm,
  runInDurableObject,
} from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { resetD1TestDatabase } from "./d1_test_utils";

type TestEnv = typeof env & {
  TEST_MIGRATIONS: Parameters<typeof applyD1Migrations>[1];
  SCHEDULER_COORDINATOR: DurableObjectNamespace;
};

let objectSequence = 0;

function coordinatorStub(): DurableObjectStub {
  const namespace = (env as TestEnv).SCHEDULER_COORDINATOR;
  const id = namespace.idFromName(`scheduler-test-${objectSequence++}`);
  return namespace.get(id);
}

async function alarmTime(stub: DurableObjectStub): Promise<number | null> {
  return runInDurableObject(stub, async (_instance, state) => state.storage.getAlarm());
}

beforeEach(async () => {
  const testEnv = env as TestEnv;
  await resetD1TestDatabase(testEnv.DB, testEnv.TEST_MIGRATIONS);
});

describe("SchedulerCoordinator Durable Object", () => {
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

  it("runs only one due job per alarm and schedules the remaining backlog", async () => {
    const now = Math.floor(Date.now() / 1000);
    await env.DB.prepare("UPDATE jobs SET next_run_at=?1, lease_until=NULL")
      .bind(now + 3600)
      .run();
    await env.DB.prepare("UPDATE jobs SET next_run_at=0 WHERE name IN ('cleanup','weather')").run();
    const stub = coordinatorStub();
    await stub.fetch("https://scheduler.internal/ensure", { method: "POST" });

    expect(await runDurableObjectAlarm(stub)).toBe(true);

    const rows = await env.DB.prepare(
      "SELECT name,next_run_at,lease_until FROM jobs WHERE name IN ('cleanup','weather') ORDER BY name",
    ).all<{ name: string; next_run_at: number; lease_until: number | null }>();
    expect(rows.results).toHaveLength(2);
    expect(rows.results[0]).toMatchObject({ name: "cleanup", lease_until: null });
    expect(Number(rows.results[0]?.next_run_at)).toBeGreaterThan(now);
    expect(rows.results[1]).toEqual({ name: "weather", next_run_at: 0, lease_until: null });

    const nextAlarm = await alarmTime(stub);
    expect(nextAlarm).not.toBeNull();
    expect(Number(nextAlarm)).toBeLessThanOrEqual(Date.now() + 5_000);
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
