import { executeSource, type Env, type SourceResult } from "./sources";
import { updateState } from "./snapshot";
import { runUpdateCheck } from "./update_check";
import { fetchStationhead } from "./spotify_source";
import { runStationheadHealthMonitor } from "./stationhead_health";
import { configuredIds, loadSwitchBotSnapshot } from "./switchbot_api";
import { fetchSwitchBotOptimized } from "./switchbot_poll";
import { failSafeSwitchBotState } from "./switchbot_state";
import type { SwitchBotEnv } from "./switchbot_types";
import { dispatchRadarBuildIfStale } from "./radar_dispatch";

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
const SUCCESS_CHECKPOINT_INTERVAL_SECONDS = 6 * 60 * 60;
const OCTOPUS_INTERVAL_SECONDS = 60 * 60 * 6;
const RADAR_DISPATCH_INTERVAL_SECONDS = 5 * 60;
const SYSTEM_JOBS_CACHE_MS = 15 * 60_000;
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

interface SystemJobsCacheEntry {
  radarEnabled: boolean;
  expiresAt: number;
  inFlight: Promise<void> | null;
}

const SYSTEM_JOBS_CACHE = new WeakMap<D1Database, SystemJobsCacheEntry>();

export function invalidateSystemJobsCache(db: D1Database): void {
  SYSTEM_JOBS_CACHE.delete(db);
}

async function reconcileSystemJobs(env: Env, nowMs: number, radarEnabled: boolean): Promise<void> {
  const octopusDeadline = Math.floor(nowMs / 1000) + OCTOPUS_INTERVAL_SECONDS;
  const radarValues = radarEnabled
    ? ",('radar_dispatch',?2,0,NULL,NULL,NULL,0)"
    : "";
  const deadlineParameter = radarEnabled ? "?3" : "?2";
  const statement = env.DB.prepare(
    `INSERT INTO jobs(
       name,interval_seconds,next_run_at,lease_until,last_success_at,last_error,consecutive_failures
     ) VALUES
       ('stationhead_health',300,0,NULL,NULL,NULL,0),
       ('octopus',?1,0,NULL,NULL,NULL,0)
       ${radarValues}
     ON CONFLICT(name) DO UPDATE SET
       interval_seconds=CASE
         WHEN excluded.name='stationhead_health' THEN jobs.interval_seconds
         ELSE excluded.interval_seconds
       END,
       next_run_at=CASE
         WHEN excluded.name='octopus' THEN CASE
           WHEN jobs.next_run_at=0 THEN 0
           WHEN jobs.next_run_at>${deadlineParameter} THEN ${deadlineParameter}
           ELSE jobs.next_run_at
         END
         ELSE jobs.next_run_at
       END
     WHERE
       (excluded.name='octopus' AND (
         jobs.interval_seconds<>excluded.interval_seconds
         OR (jobs.next_run_at<>0 AND jobs.next_run_at>${deadlineParameter})
       ))
       OR (excluded.name='radar_dispatch' AND jobs.interval_seconds<>excluded.interval_seconds)`,
  );
  if (radarEnabled) {
    await statement.bind(
      OCTOPUS_INTERVAL_SECONDS,
      RADAR_DISPATCH_INTERVAL_SECONDS,
      octopusDeadline,
    ).run();
  } else {
    await env.DB.batch([
      statement.bind(OCTOPUS_INTERVAL_SECONDS, octopusDeadline),
      env.DB.prepare("DELETE FROM jobs WHERE name='radar_dispatch'"),
    ]);
  }
}

export async function ensureSystemJobs(env: Env, nowMs = Date.now()): Promise<void> {
  const radarEnabled = Boolean(env.GITHUB_RADAR_DISPATCH_TOKEN?.trim());
  const cached = SYSTEM_JOBS_CACHE.get(env.DB);
  if (cached?.radarEnabled === radarEnabled) {
    if (cached.inFlight) return cached.inFlight;
    if (cached.expiresAt > nowMs) return;
  }

  const entry: SystemJobsCacheEntry = { radarEnabled, expiresAt: 0, inFlight: null };
  const task = reconcileSystemJobs(env, nowMs, radarEnabled).then(
    () => {
      if (SYSTEM_JOBS_CACHE.get(env.DB) === entry) {
        entry.expiresAt = nowMs + SYSTEM_JOBS_CACHE_MS;
        entry.inFlight = null;
      }
    },
    error => {
      if (SYSTEM_JOBS_CACHE.get(env.DB) === entry) SYSTEM_JOBS_CACHE.delete(env.DB);
      throw error;
    },
  );
  entry.inFlight = task;
  SYSTEM_JOBS_CACHE.set(env.DB, entry);
  return task;
}

export async function acquireDueJobs(
  env: Env,
  nowSeconds: number,
  limit = MAX_PARALLEL,
): Promise<JobRow[]> {
  const batchSize = Math.max(1, Math.min(MAX_PARALLEL, Math.trunc(limit)));
  const result = await env.DB.prepare(
    `WITH due AS (
       SELECT name FROM jobs
        WHERE next_run_at <= ?1 AND (lease_until IS NULL OR lease_until < ?1)
        ORDER BY next_run_at, name LIMIT ?2
     )
     UPDATE jobs SET
       lease_until = ?3,
       next_run_at = CASE
         WHEN next_run_at=0 THEN -(?1 + interval_seconds)
         ELSE ?1 + interval_seconds
       END
      WHERE name IN (SELECT name FROM due)
        AND next_run_at <= ?1 AND (lease_until IS NULL OR lease_until < ?1)
     RETURNING name, interval_seconds, next_run_at, lease_until, last_success_at, consecutive_failures`,
  ).bind(nowSeconds, batchSize, nowSeconds + LEASE_SECONDS).all<JobRow>();
  return result.results ?? [];
}

export async function finishJob(
  env: Env,
  job: JobRow,
  startedAt: number,
  success: boolean,
  error?: string,
  nowSeconds = Math.floor(Date.now() / 1000),
): Promise<boolean> {
  const failures = success ? 0 : job.consecutive_failures + 1;
  const retrySeconds = success
    ? job.interval_seconds
    : Math.min(job.interval_seconds, Math.max(60, 60 * 2 ** Math.min(4, failures - 1)));
  const checkpointSuccess = success && (
    job.next_run_at < 0
    || job.last_success_at === null
    || job.consecutive_failures > 0
    || startedAt - job.last_success_at >= SUCCESS_CHECKPOINT_INTERVAL_SECONDS
    || Number(job.lease_until ?? 0) <= nowSeconds
  );
  if (success && !checkpointSuccess) {
    // The regular next run was already scheduled atomically with lease acquisition.
    // Release the completed lease even when a refresh was not queued; otherwise the
    // coordinator schedules a redundant lease-expiry alarm and a later refresh waits.
    const released = await env.DB.prepare(
      `UPDATE jobs SET lease_until=NULL
        WHERE name=?1 AND lease_until=?2`,
    ).bind(job.name, job.lease_until).run();
    return Number(released.meta.changes ?? 0) === 1;
  }

  const update = await env.DB.prepare(
    `UPDATE jobs SET
       next_run_at=CASE
         WHEN next_run_at=0 THEN 0
         WHEN ?2=1 AND next_run_at<0 THEN -next_run_at
         WHEN ?2=1 THEN next_run_at
         ELSE ?1
       END,
       lease_until=NULL,
       last_success_at=CASE WHEN ?2=1 THEN ?3 ELSE last_success_at END,
       last_error=?4,
       consecutive_failures=?5
     WHERE name=?6 AND lease_until=?7`,
  ).bind(
    nowSeconds + retrySeconds,
    success ? 1 : 0,
    nowSeconds,
    success ? null : error ?? "unknown error",
    failures,
    job.name,
    job.lease_until,
  ).run();
  if (Number(update.meta.changes ?? 0) !== 1) return false;

  const shouldLogRun = !success || job.last_success_at === null
    || startedAt - job.last_success_at >= SUCCESS_CHECKPOINT_INTERVAL_SECONDS;
  if (shouldLogRun) {
    await env.DB.prepare(
      "INSERT INTO job_runs(job_name,started_at,finished_at,success,detail) VALUES(?1,?2,?3,?4,?5)",
    ).bind(job.name, startedAt, nowSeconds, success ? 1 : 0, error ?? null).run();
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
    else if (job.name === "radar_dispatch") await dispatchRadarBuildIfStale(env);
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
      } else if (!["cleanup", "update_check", "radar_dispatch"].includes(job.name) && !sourceFailureRecorded) {
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
    if (jobs.length < MAX_PARALLEL) return;
  }
  console.error("Scheduler stopped after the safety batch limit with due work possibly remaining");
}

export async function runSchedulerTick(env: Env): Promise<void> {
  await ensureSystemJobs(env);
  const now = Math.floor(Date.now() / 1000);
  const jobs = await acquireDueJobs(env, now, 1);
  const job = jobs[0];
  if (!job) return;
  try {
    await runOne(env, job);
  } catch (error) {
    console.error(`Scheduled job ${job.name} failed to finalize`, error);
  }
}

export async function requestRefresh(env: Env, names?: string[]): Promise<boolean> {
  const selected = names === undefined
    ? [...REFRESHABLE_JOBS]
    : [...new Set(names.filter(name => REFRESHABLE_JOB_SET.has(name)))];
  if (!selected.length) return false;
  await ensureSystemJobs(env);
  const placeholders = selected.map(() => "?").join(",");
  await env.DB.prepare(
    `UPDATE jobs SET next_run_at=0
      WHERE name IN (${placeholders}) AND next_run_at<>0`,
  ).bind(...selected).run();
  return true;
}
