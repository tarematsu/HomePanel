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

const MIN_RETRY_SECONDS = 60;
const MAX_FAILURE_EXPONENT = 4;
const RUN_LOG_CHECKPOINT_SECONDS = 6 * 60 * 60;
const NON_SOURCE_JOBS = new Set<string>([
  "cleanup",
  "update_check",
  LIVENESS_JOB_NAME,
]);

interface JobExecution {
  startedAt: number;
  success: boolean;
  message?: string;
}

export async function selectDueJob(env: Env, nowSeconds: number): Promise<JobRow | null> {
  return env.DB.prepare(
    `SELECT name,interval_seconds,next_run_at,lease_until,last_success_at,consecutive_failures
       FROM jobs
      WHERE next_run_at<=?1 AND (lease_until IS NULL OR lease_until<?1)
      ORDER BY next_run_at,name
      LIMIT 1`,
  ).bind(nowSeconds).first<JobRow>();
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

async function executeJob(env: Env, job: JobRow): Promise<JobExecution> {
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

export async function finishSelectedJob(
  env: Env,
  job: JobRow,
  execution: JobExecution,
  nowSeconds = Math.floor(Date.now() / 1000),
): Promise<boolean> {
  const failures = execution.success ? 0 : job.consecutive_failures + 1;
  const retrySeconds = execution.success
    ? job.interval_seconds
    : Math.min(
      job.interval_seconds,
      Math.max(MIN_RETRY_SECONDS, MIN_RETRY_SECONDS * 2 ** Math.min(MAX_FAILURE_EXPONENT, failures - 1)),
    );
  const nextRunAt = nowSeconds + retrySeconds;
  const update = await env.DB.prepare(
    `UPDATE jobs SET
       next_run_at=?1,
       lease_until=NULL,
       last_success_at=CASE WHEN ?2=1 THEN ?3 ELSE last_success_at END,
       last_error=CASE WHEN ?2=1 THEN NULL ELSE ?4 END,
       consecutive_failures=?5
     WHERE name=?6
       AND next_run_at=?7
       AND (lease_until IS NULL OR lease_until<?8)`,
  ).bind(
    nextRunAt,
    execution.success ? 1 : 0,
    execution.startedAt,
    execution.message ?? "unknown error",
    failures,
    job.name,
    job.next_run_at,
    nowSeconds,
  ).run();
  if (Number(update.meta.changes ?? 0) !== 1) return false;

  const shouldLog = !execution.success
    || job.last_success_at === null
    || execution.startedAt - job.last_success_at >= RUN_LOG_CHECKPOINT_SECONDS;
  if (shouldLog) {
    await env.DB.prepare(
      "INSERT INTO job_runs(job_name,started_at,finished_at,success,detail) VALUES(?1,?2,?3,?4,?5)",
    ).bind(
      job.name,
      execution.startedAt,
      nowSeconds,
      execution.success ? 1 : 0,
      execution.message ?? null,
    ).run();
  }
  return true;
}

export async function runSchedulerTickLowWrite(env: Env): Promise<void> {
  await ensureSystemJobs(env);
  const nowSeconds = Math.floor(Date.now() / 1000);
  const job = await selectDueJob(env, nowSeconds);
  if (!job) return;
  const execution = await executeJob(env, job);
  const finalized = await finishSelectedJob(env, job, execution);
  if (!finalized) {
    console.log("Scheduled job completion preserved a concurrent refresh", { job: job.name });
  }
}
