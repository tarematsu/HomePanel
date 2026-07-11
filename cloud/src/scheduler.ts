import { executeSource, type Env, type SourceResult } from "./sources";
import { updateState } from "./snapshot";
import { runUpdateCheck } from "./update_check";
import { fetchStationhead } from "./spotify_source";
import { runStationheadHealthMonitor } from "./stationhead_health";
import { configuredIds, loadSwitchBotSnapshot } from "./switchbot_api";
import { fetchSwitchBotOptimized } from "./switchbot_poll";
import { failSafeSwitchBotState } from "./switchbot_state";
import type { SwitchBotEnv } from "./switchbot_types";

export interface JobRow {
  name: string;
  interval_seconds: number;
  next_run_at: number;
  lease_until: number | null;
  last_success_at: number | null;
  consecutive_failures: number;
}

const LEASE_SECONDS = 120;
const MAX_PARALLEL = 3;
const MAX_SCHEDULER_BATCHES = 10;
const SUCCESS_RUN_LOG_INTERVAL_SECONDS = 6 * 60 * 60;
const OCTOPUS_INTERVAL_SECONDS = 60 * 60;
const DAY_MS = 86_400_000;
const DAY_SECONDS = 86_400;
const POWER_RETENTION_MS = 90 * DAY_MS;
const REFRESHABLE_JOBS = [
  "weather",
  "news",
  "switchbot",
  "octopus",
  "stationhead",
  "stationhead_health",
  "radar",
  "update_check",
] as const;
const REFRESHABLE_JOB_SET = new Set<string>(REFRESHABLE_JOBS);

export async function ensureSystemJobs(env: Env): Promise<void> {
  const octopusDeadline = Math.floor(Date.now() / 1000) + OCTOPUS_INTERVAL_SECONDS;
  await env.DB.batch([
    env.DB.prepare(
      `INSERT OR IGNORE INTO jobs(
         name,interval_seconds,next_run_at,lease_until,last_success_at,last_error,consecutive_failures
       ) VALUES('stationhead_health',300,0,NULL,NULL,NULL,0)`,
    ),
    env.DB.prepare(
      `INSERT INTO jobs(
         name,interval_seconds,next_run_at,lease_until,last_success_at,last_error,consecutive_failures
       ) VALUES('octopus',?1,0,NULL,NULL,NULL,0)
       ON CONFLICT(name) DO UPDATE SET
         interval_seconds=excluded.interval_seconds,
         next_run_at=CASE
           WHEN jobs.next_run_at=0 THEN 0
           WHEN jobs.next_run_at>?2 THEN ?2
           ELSE jobs.next_run_at
         END`,
    ).bind(OCTOPUS_INTERVAL_SECONDS, octopusDeadline),
  ]);
}

export async function acquireDueJobs(env: Env, nowSeconds: number): Promise<JobRow[]> {
  const result = await env.DB.prepare(
    `WITH due AS (
       SELECT name FROM jobs
        WHERE next_run_at <= ?1 AND (lease_until IS NULL OR lease_until < ?1)
        ORDER BY next_run_at, name LIMIT ?2
     )
     UPDATE jobs SET
       lease_until = ?3,
       next_run_at = CASE WHEN next_run_at=0 THEN -1 ELSE next_run_at END
      WHERE name IN (SELECT name FROM due)
        AND next_run_at <= ?1 AND (lease_until IS NULL OR lease_until < ?1)
     RETURNING name, interval_seconds, next_run_at, lease_until, last_success_at, consecutive_failures`,
  ).bind(nowSeconds, MAX_PARALLEL, nowSeconds + LEASE_SECONDS).all<JobRow>();
  return result.results ?? [];
}

export async function finishJob(env: Env, job: JobRow, startedAt: number, success: boolean, error?: string): Promise<boolean> {
  const now = Math.floor(Date.now() / 1000);
  const failures = success ? 0 : job.consecutive_failures + 1;
  const retrySeconds = success
    ? job.interval_seconds
    : Math.min(job.interval_seconds, Math.max(60, 60 * 2 ** Math.min(4, failures - 1)));
  const preserveQueuedRefresh = job.next_run_at !== 0 ? 1 : 0;
  const update = await env.DB.prepare(
    `UPDATE jobs SET
       next_run_at=CASE WHEN ?7=1 AND next_run_at=0 THEN 0 ELSE ?1 END,
       lease_until=NULL,
       last_success_at=CASE WHEN ?2=1 THEN ?3 ELSE last_success_at END,
       last_error=?4,
       consecutive_failures=?5
     WHERE name=?6 AND lease_until=?8`,
  ).bind(
    now + retrySeconds,
    success ? 1 : 0,
    now,
    success ? null : error ?? "unknown error",
    failures,
    job.name,
    preserveQueuedRefresh,
    job.lease_until,
  ).run();
  if (Number(update.meta.changes ?? 0) !== 1) return false;

  const shouldLogRun = !success || job.last_success_at === null
    || startedAt - job.last_success_at >= SUCCESS_RUN_LOG_INTERVAL_SECONDS;
  if (shouldLogRun) {
    await env.DB.prepare(
      "INSERT INTO job_runs(job_name,started_at,finished_at,success,detail) VALUES(?1,?2,?3,?4,?5)",
    ).bind(job.name, startedAt, now, success ? 1 : 0, error ?? null).run();
  }
  return true;
}

export async function cleanupExpiredData(env: Env, now = Date.now()): Promise<void> {
  await env.DB.batch([
    env.DB.prepare("DELETE FROM power_samples WHERE observed_at < ?1").bind(now - POWER_RETENTION_MS),
    env.DB.prepare(
      `DELETE FROM device_commands
        WHERE (completed_at IS NOT NULL AND completed_at < ?1)
           OR (completed_at IS NULL AND expires_at IS NOT NULL AND expires_at < ?2)`,
    ).bind(now - 30 * DAY_MS, now - 7 * DAY_MS),
    env.DB.prepare("DELETE FROM job_runs WHERE finished_at < ?1")
      .bind(Math.floor(now / 1000) - 30 * DAY_SECONDS),
  ]);
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

async function runOne(env: Env, job: JobRow): Promise<void> {
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
      } else if (job.name !== "cleanup" && job.name !== "update_check" && !sourceFailureRecorded) {
        await updateState(env, { source: job.name, payload: null, observedAt: Date.now() }, message);
      }
    } catch (stateError) {
      console.error(`Failed to record ${job.name} error state`, stateError instanceof Error ? stateError.message : String(stateError));
    }
  }
  try {
    await finishJob(env, job, startedAt, success, message);
  } catch (finishError) {
    console.error(`Failed to finish ${job.name} job`, finishError instanceof Error ? finishError.message : String(finishError));
    throw finishError;
  }
}

export async function runScheduler(env: Env): Promise<void> {
  await ensureSystemJobs(env);
  for (let batch = 0; batch < MAX_SCHEDULER_BATCHES; batch += 1) {
    const now = Math.floor(Date.now() / 1000);
    const jobs = await acquireDueJobs(env, now);
    if (!jobs.length) return;
    const results = await Promise.allSettled(jobs.map(job => runOne(env, job)));
    results.forEach((result, index) => {
      if (result.status === "rejected") console.error(`Scheduled job ${jobs[index]?.name ?? index} failed to finalize`, result.reason);
    });
  }
  console.error("Scheduler stopped after the safety batch limit with due work possibly remaining");
}

export async function requestRefresh(env: Env, names?: string[]): Promise<void> {
  await ensureSystemJobs(env);
  const selected = names?.length
    ? [...new Set(names.filter(name => REFRESHABLE_JOB_SET.has(name)))]
    : [...REFRESHABLE_JOBS];
  if (!selected.length) return;
  const placeholders = selected.map(() => "?").join(",");
  await env.DB.prepare(
    `UPDATE jobs SET next_run_at=0
      WHERE name IN (${placeholders}) AND next_run_at<>0`,
  ).bind(...selected).run();
}
