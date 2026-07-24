import { executeSource, type Env, type SourceResult } from "./sources";
import { updateState } from "./snapshot";
import { cleanupExpiredData, ensureSystemJobs, type JobRow } from "./scheduler";
import { runUpdateCheck } from "./update_check";
import { fetchStationhead } from "./spotify_source";
import { runStationheadHealthMonitor } from "./stationhead_health";
import { configuredIds, loadSwitchBotSnapshot } from "./switchbot_api";
import { fetchSwitchBotOptimized } from "./switchbot_poll";
import { failSafeSwitchBotState } from "./switchbot_state";
import type { SwitchBotEnv } from "./switchbot_types";
import { runVideoLiveness } from "./video_liveness";
import { LIVENESS_JOB_NAME } from "../../video/src/liveness-schedule.js";

const RUNTIME_STORAGE_KEY = "scheduler-runtime-v2";
const RUNTIME_VERSION = 2;
const MIN_RETRY_SECONDS = 60;
const MAX_FAILURE_EXPONENT = 4;
const EMPTY_RECHECK_SECONDS = 24 * 60 * 60;
const MAX_RUNTIME_BATCH = 3;
const NON_SOURCE_JOBS = new Set<string>([
  "cleanup",
  "update_check",
  LIVENESS_JOB_NAME,
]);

interface RuntimeJob {
  name: string;
  intervalSeconds: number;
  nextRunAt: number;
  lastSuccessAt: number | null;
  consecutiveFailures: number;
  lastError: string | null;
}

interface RuntimeEnvelope {
  version: number;
  jobs: RuntimeJob[];
}

interface JobExecution {
  startedAt: number;
  success: boolean;
  message?: string;
}

interface PendingJobEvent {
  jobName: string;
  occurredAt: number;
  event: "failed" | "recovered";
  detail: string | null;
}

function normalizedJob(row: JobRow, nowSeconds: number): RuntimeJob {
  const configuredNext = Number(row.next_run_at);
  return {
    name: row.name,
    intervalSeconds: Math.max(MIN_RETRY_SECONDS, Number(row.interval_seconds) || MIN_RETRY_SECONDS),
    nextRunAt: configuredNext > nowSeconds ? configuredNext : nowSeconds,
    lastSuccessAt: row.last_success_at === null ? null : Number(row.last_success_at),
    consecutiveFailures: Math.max(0, Number(row.consecutive_failures) || 0),
    lastError: null,
  };
}

async function bootstrapRuntime(
  state: DurableObjectState,
  env: Env,
  nowSeconds: number,
): Promise<RuntimeEnvelope> {
  await ensureSystemJobs(env, nowSeconds * 1000);
  const result = await env.DB.prepare(
    `SELECT name,interval_seconds,next_run_at,lease_until,last_success_at,consecutive_failures
       FROM jobs ORDER BY name`,
  ).all<JobRow>();
  const envelope: RuntimeEnvelope = {
    version: RUNTIME_VERSION,
    jobs: (result.results ?? []).map(row => normalizedJob(row, nowSeconds)),
  };
  await state.storage.put(RUNTIME_STORAGE_KEY, envelope);
  return envelope;
}

async function runtimeEnvelope(
  state: DurableObjectState,
  env: Env,
  nowSeconds = Math.floor(Date.now() / 1000),
): Promise<RuntimeEnvelope> {
  const stored = await state.storage.get<RuntimeEnvelope>(RUNTIME_STORAGE_KEY);
  if (stored?.version === RUNTIME_VERSION && Array.isArray(stored.jobs)) return stored;
  return bootstrapRuntime(state, env, nowSeconds);
}

async function recordSourceFailure(env: Env, source: string, error: unknown): Promise<string> {
  const message = error instanceof Error ? error.message.slice(0, 1000) : String(error).slice(0, 1000);
  await updateState(env, { source, payload: null, observedAt: Date.now() }, message);
  return `${source}: ${message}`;
}

async function refreshStationheadMonitor(env: Env): Promise<void> {
  try {
    await updateState(env, await fetchStationhead(env));
  } catch (error) {
    throw new Error(await recordSourceFailure(env, "stationhead", error));
  }
}

async function executeRuntimeJob(env: Env, job: RuntimeJob): Promise<JobExecution> {
  const startedAt = Math.floor(Date.now() / 1000);
  let success = false;
  let message: string | undefined;
  let sourceFailureRecorded = false;
  try {
    if (job.name === "cleanup") await cleanupExpiredData(env);
    else if (job.name === "update_check") await runUpdateCheck(env);
    else if (job.name === "stationhead") {
      sourceFailureRecorded = true;
      await refreshStationheadMonitor(env);
    } else if (job.name === "stationhead_health") {
      await runStationheadHealthMonitor(env);
    } else if (job.name === LIVENESS_JOB_NAME) {
      await runVideoLiveness(env);
    } else {
      const result: SourceResult = job.name === "switchbot"
        ? await fetchSwitchBotOptimized(env)
        : await executeSource(job.name, env);
      await updateState(env, result);
    }
    success = true;
  } catch (error) {
    message = error instanceof Error ? error.message.slice(0, 1000) : String(error).slice(0, 1000);
    try {
      if (job.name === "switchbot") {
        const switchbotEnv = env as SwitchBotEnv;
        const now = Date.now();
        const snapshot = await loadSwitchBotSnapshot(switchbotEnv);
        const controlPlugIds = configuredIds(switchbotEnv.SWITCHBOT_CONTROL_PLUG_IDS);
        await updateState(env, {
          source: "switchbot",
          payload: failSafeSwitchBotState(snapshot.state, now, controlPlugIds, message),
          observedAt: now,
        }, undefined, snapshot.row);
      } else if (!NON_SOURCE_JOBS.has(job.name) && !sourceFailureRecorded) {
        await updateState(env, { source: job.name, payload: null, observedAt: Date.now() }, message);
      }
    } catch (stateError) {
      console.error(`Failed to record ${job.name} error state`, stateError instanceof Error
        ? stateError.message
        : String(stateError));
    }
  }
  return { startedAt, success, ...(message === undefined ? {} : { message }) };
}

function transitionEvent(
  job: RuntimeJob,
  execution: JobExecution,
  completedAt: number,
): PendingJobEvent | null {
  if (!execution.success && job.consecutiveFailures === 0) {
    return {
      jobName: job.name,
      occurredAt: completedAt,
      event: "failed",
      detail: execution.message ?? "unknown error",
    };
  }
  if (execution.success && job.consecutiveFailures > 0) {
    return {
      jobName: job.name,
      occurredAt: completedAt,
      event: "recovered",
      detail: null,
    };
  }
  return null;
}

async function recordJobEventsBestEffort(env: Env, events: readonly PendingJobEvent[]): Promise<void> {
  if (!events.length) return;
  const statement = env.DB.prepare(
    `INSERT INTO job_events(job_name,occurred_at,event,detail)
     VALUES(?1,?2,?3,?4)
     ON CONFLICT(job_name,occurred_at,event) DO NOTHING`,
  );
  try {
    await env.DB.batch(events.map(event => statement.bind(
      event.jobName,
      event.occurredAt,
      event.event,
      event.detail,
    )));
  } catch (error) {
    console.error("Failed to record scheduler transition events", error instanceof Error
      ? error.message
      : String(error));
  }
}

function dueJobs(
  envelope: RuntimeEnvelope,
  nowSeconds: number,
  limit = MAX_RUNTIME_BATCH,
): RuntimeJob[] {
  return envelope.jobs
    .filter(job => job.nextRunAt <= nowSeconds)
    .sort((left, right) => left.nextRunAt - right.nextRunAt || left.name.localeCompare(right.name))
    .slice(0, Math.max(1, Math.trunc(limit)));
}

export async function runtimeNextWakeAt(
  state: DurableObjectState,
  env: Env,
  nowMs = Date.now(),
): Promise<number> {
  const nowSeconds = Math.floor(nowMs / 1000);
  const envelope = await runtimeEnvelope(state, env, nowSeconds);
  let next = Number.POSITIVE_INFINITY;
  for (const job of envelope.jobs) next = Math.min(next, job.nextRunAt);
  return Number.isFinite(next) ? next * 1000 : nowMs + EMPTY_RECHECK_SECONDS * 1000;
}

export async function refreshRuntimeJobs(
  state: DurableObjectState,
  env: Env,
  names?: readonly string[],
  nowSeconds = Math.floor(Date.now() / 1000),
): Promise<number> {
  const envelope = await runtimeEnvelope(state, env, nowSeconds);
  const selected = names === undefined ? new Set(envelope.jobs.map(job => job.name)) : new Set(names);
  let changed = 0;
  for (const job of envelope.jobs) {
    if (!selected.has(job.name)) continue;
    if (job.nextRunAt !== nowSeconds) {
      job.nextRunAt = nowSeconds;
      changed += 1;
    }
  }
  if (changed) await state.storage.put(RUNTIME_STORAGE_KEY, envelope);
  return changed;
}

export async function runRuntimeSchedulerTick(
  state: DurableObjectState,
  env: Env,
  nowSeconds = Math.floor(Date.now() / 1000),
): Promise<string[]> {
  const envelope = await runtimeEnvelope(state, env, nowSeconds);
  const jobs = dueJobs(envelope, nowSeconds);
  if (!jobs.length) return [];

  const executions = await Promise.all(jobs.map(job => executeRuntimeJob(env, job)));
  const completedAt = Math.floor(Date.now() / 1000);
  const events: PendingJobEvent[] = [];

  for (let index = 0; index < jobs.length; index += 1) {
    const job = jobs[index]!;
    const execution = executions[index]!;
    const event = transitionEvent(job, execution, completedAt);
    if (event) events.push(event);

    if (execution.success) {
      job.nextRunAt = completedAt + job.intervalSeconds;
      job.lastSuccessAt = execution.startedAt;
      job.consecutiveFailures = 0;
      job.lastError = null;
    } else {
      job.consecutiveFailures += 1;
      const retrySeconds = Math.min(
        job.intervalSeconds,
        Math.max(
          MIN_RETRY_SECONDS,
          MIN_RETRY_SECONDS * 2 ** Math.min(MAX_FAILURE_EXPONENT, job.consecutiveFailures - 1),
        ),
      );
      job.nextRunAt = completedAt + retrySeconds;
      job.lastError = execution.message ?? "unknown error";
    }
  }

  // Runtime progress is authoritative. Diagnostic D1 history is deliberately
  // recorded only after the durable checkpoint and cannot block future alarms.
  await state.storage.put(RUNTIME_STORAGE_KEY, envelope);
  await recordJobEventsBestEffort(env, events);
  return jobs.map(job => job.name);
}

export async function resetRuntimeFromD1(
  state: DurableObjectState,
  env: Env,
  nowSeconds = Math.floor(Date.now() / 1000),
): Promise<void> {
  await state.storage.delete(RUNTIME_STORAGE_KEY);
  await bootstrapRuntime(state, env, nowSeconds);
}
