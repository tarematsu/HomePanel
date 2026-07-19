import { ensureSystemJobs, runSchedulerTick } from "./scheduler";
import type { Env } from "./sources";

const COORDINATOR_NAME = "global";
const ENSURE_THROTTLE_MS = 15 * 60_000;
const MIN_ALARM_DELAY_MS = 1_000;
const RECOVERY_ALARM_DELAY_MS = 60_000;
const EMPTY_QUEUE_RECHECK_MS = 15 * 60_000;

interface SchedulerEnv extends Env {
  SCHEDULER_COORDINATOR?: DurableObjectNamespace;
}

interface NextWakeRow {
  wake_at: number | null;
}

let nextEnsureAllowedAt = 0;

function namespaceFor(env: Env): DurableObjectNamespace | null {
  return (env as SchedulerEnv).SCHEDULER_COORDINATOR ?? null;
}

function coordinatorStub(env: Env): DurableObjectStub | null {
  const namespace = namespaceFor(env);
  if (!namespace) return null;
  return namespace.get(namespace.idFromName(COORDINATOR_NAME));
}

async function signalCoordinator(env: Env, path: "/ensure" | "/wake"): Promise<void> {
  const stub = coordinatorStub(env);
  if (!stub) return;
  const response = await stub.fetch(`https://scheduler.internal${path}`, { method: "POST" });
  if (!response.ok) throw new Error(`scheduler coordinator ${path} failed: HTTP ${response.status}`);
}

export function queueSchedulerEnsure(
  env: Env,
  ctx: ExecutionContext,
  now = Date.now(),
): boolean {
  if (!namespaceFor(env) || now < nextEnsureAllowedAt) return false;
  nextEnsureAllowedAt = now + ENSURE_THROTTLE_MS;
  ctx.waitUntil(signalCoordinator(env, "/ensure").catch(error => {
    nextEnsureAllowedAt = 0;
    console.error("Failed to ensure scheduler alarm", error instanceof Error ? error.message : String(error));
  }));
  return true;
}

export function queueSchedulerWake(env: Env, ctx: ExecutionContext): boolean {
  if (!namespaceFor(env)) return false;
  ctx.waitUntil(signalCoordinator(env, "/wake").catch(error => {
    console.error("Failed to wake scheduler alarm", error instanceof Error ? error.message : String(error));
  }));
  return true;
}

export class SchedulerCoordinator {
  constructor(
    private readonly state: DurableObjectState,
    private readonly env: Env,
  ) {}

  private async nextWakeAt(nowMs = Date.now()): Promise<number> {
    await ensureSystemJobs(this.env, nowMs);
    const nowSeconds = Math.floor(nowMs / 1000);
    const row = await this.env.DB.prepare(
      `SELECT MIN(
         CASE
           WHEN lease_until IS NOT NULL AND lease_until>=?1 THEN lease_until+1
           WHEN next_run_at<=?1 THEN ?1
           ELSE next_run_at
         END
       ) AS wake_at
       FROM jobs`,
    ).bind(nowSeconds).first<NextWakeRow>();
    const wakeAtSeconds = Number(row?.wake_at);
    if (!Number.isFinite(wakeAtSeconds)) return nowMs + EMPTY_QUEUE_RECHECK_MS;
    return Math.max(nowMs + MIN_ALARM_DELAY_MS, wakeAtSeconds * 1000);
  }

  private async setEarlierAlarm(desiredAt: number): Promise<number> {
    const current = await this.state.storage.getAlarm();
    if (current === null || desiredAt < current) {
      await this.state.storage.setAlarm(desiredAt);
      return desiredAt;
    }
    return current;
  }

  private async scheduleNext(): Promise<number> {
    return this.setEarlierAlarm(await this.nextWakeAt());
  }

  async fetch(request: Request): Promise<Response> {
    if (request.method !== "POST") {
      return Response.json({ error: "method_not_allowed" }, {
        status: 405,
        headers: { Allow: "POST" },
      });
    }
    const path = new URL(request.url).pathname;
    if (path === "/wake") {
      const alarmAt = await this.setEarlierAlarm(Date.now() + MIN_ALARM_DELAY_MS);
      return Response.json({ scheduled: true, alarmAt }, { status: 202 });
    }
    if (path === "/ensure") {
      const alarmAt = await this.scheduleNext();
      return Response.json({ scheduled: true, alarmAt }, { status: 202 });
    }
    return Response.json({ error: "not_found" }, { status: 404 });
  }

  async alarm(): Promise<void> {
    try {
      await runSchedulerTick(this.env);
    } catch (error) {
      console.error("Scheduler alarm tick failed", error instanceof Error ? error.message : String(error));
    }

    try {
      await this.scheduleNext();
    } catch (error) {
      console.error("Failed to schedule the next scheduler alarm", error instanceof Error ? error.message : String(error));
      await this.state.storage.setAlarm(Date.now() + RECOVERY_ALARM_DELAY_MS);
    }
  }
}
