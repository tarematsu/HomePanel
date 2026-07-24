import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';

const cloudConfig = JSON.parse(readFileSync(new URL('../../cloud/wrangler.jsonc', import.meta.url), 'utf8'));
const nativeConfig = readFileSync(new URL('../../native/src/config.h', import.meta.url), 'utf8');
const nativeCloudConfig = readFileSync(new URL('../../native/src/cloud_config.cpp', import.meta.url), 'utf8');
const adminPage = readFileSync(new URL('../../cloud/src/admin.ts', import.meta.url), 'utf8');
const deviceExchange = readFileSync(new URL('../../cloud/src/device_exchange.ts', import.meta.url), 'utf8');
const octopusHistory = readFileSync(new URL('../../cloud/src/octopus_history.ts', import.meta.url), 'utf8');
const schedulerRuntime = readFileSync(new URL('../../cloud/src/scheduler_runtime.ts', import.meta.url), 'utf8');
const telemetryHeartbeat = readFileSync(new URL('../../cloud/src/telemetry_heartbeat.ts', import.meta.url), 'utf8');
const resourceMigration = readFileSync(
  new URL('../../cloud/migrations/202607220100_resource_budget_3000.sql', import.meta.url),
  'utf8'
);
const octopusScheduleMigration = readFileSync(
  new URL('../../cloud/migrations/202607240100_octopus_daily_stable_only.sql', import.meta.url),
  'utf8'
);
const runtimeMigration = readFileSync(
  new URL('../../cloud/migrations/202607240200_d1_runtime_reduction.sql', import.meta.url),
  'utf8'
);
const runtimeBugfixMigration = readFileSync(
  new URL('../../cloud/migrations/202607240300_d1_runtime_bugfixes.sql', import.meta.url),
  'utf8'
);
const livenessSchedule = readFileSync(new URL('../src/liveness-schedule.js', import.meta.url), 'utf8');

const DAY_SECONDS = 86_400;
const TARGET = 3_000;
const STATE_HEARTBEAT_SECONDS = 6 * 60 * 60;
const SCHEDULER_BATCH_SIZE = 3;
const scheduledIntervals = {
  switchbot: 900,
  stationhead: 900,
  stationhead_health: 1_800,
  news: 1_800,
  weather: 3_600,
  octopus: 86_400,
  video_liveness: 3_600,
  update_check: 21_600,
  cleanup: 86_400
};
const heartbeatStateIntervals = [900, 1_800, 1_800, 3_600, 86_400];

function runsPerDay(intervalSeconds) {
  return Math.ceil(DAY_SECONDS / intervalSeconds);
}

function throttledHeartbeatWrites(intervalSeconds) {
  const effectiveInterval = Math.ceil(STATE_HEARTBEAT_SECONDS / intervalSeconds) * intervalSeconds;
  return runsPerDay(effectiveInterval);
}

function modeledSchedulerAlarms() {
  const nextRunAt = Object.fromEntries(Object.keys(scheduledIntervals).map((name) => [name, 0]));
  let alarms = 0;
  while (true) {
    const next = Math.min(...Object.values(nextRunAt));
    if (next >= DAY_SECONDS) return alarms;
    const due = Object.keys(nextRunAt)
      .filter((name) => nextRunAt[name] <= next)
      .sort()
      .slice(0, SCHEDULER_BATCH_SIZE);
    alarms += 1;
    for (const name of due) nextRunAt[name] = next + scheduledIntervals[name];
  }
}

test('static assets bypass the Worker while dynamic routes remain Worker-first', () => {
  assert.deepEqual(cloudConfig.assets.run_worker_first, ['/api/*', '/v1/*', '/admin']);
  assert.notEqual(cloudConfig.assets.run_worker_first, true);
});

test('Workers KV is absent while bounded R2 caches remain declared', () => {
  assert.equal(cloudConfig.kv_namespaces, undefined);
  assert.ok(cloudConfig.r2_buckets.some((entry) => entry.binding === 'DATA_BUCKET'));
});

test('native polling is fixed at thirty-minute sync and two-hour telemetry', () => {
  assert.match(nativeConfig, /cloudPollSeconds = 1800;/);
  assert.match(nativeConfig, /telemetryMinutes = 120;/);
  assert.match(nativeCloudConfig, /config\.cloudPollSeconds = 1800;/);
  assert.match(nativeCloudConfig, /config\.telemetryMinutes = 120;/);
  assert.match(adminPage, /cloudPollSeconds:1800,telemetryMinutes:120/);
  assert.match(adminPage, /config\.cloudPollSeconds=1800;config\.telemetryMinutes=120/);
});

test('scheduler uses batched DO runtime and hourly five-video liveness batches', () => {
  for (const [name, interval] of Object.entries(scheduledIntervals)) {
    if (name === 'octopus') {
      assert.match(octopusScheduleMigration, /interval_seconds = 86400/);
      assert.match(octopusScheduleMigration, /WHERE name = 'octopus'/);
      continue;
    }
    if (name === 'video_liveness') {
      assert.match(runtimeMigration, /interval_seconds=3600/);
      assert.match(runtimeMigration, /WHERE name='video_liveness'/);
      continue;
    }
    assert.match(resourceMigration, new RegExp(`WHEN '${name}' THEN ${interval}`));
  }
  assert.match(livenessSchedule, /LIVENESS_INTERVAL_SECONDS = 60 \* 60/);
  assert.match(schedulerRuntime, /MAX_RUNTIME_BATCH = 3/);
  assert.match(schedulerRuntime, /Promise\.all\(jobs\.map/);
  assert.match(schedulerRuntime, /recordJobEventsBestEffort/);
  assert.match(schedulerRuntime, /state\.storage\.put\(RUNTIME_STORAGE_KEY/);
  assert.doesNotMatch(schedulerRuntime, /UPDATE jobs SET/);
});

test('device exchange merges telemetry before the sync snapshot read', () => {
  const mergeAt = deviceExchange.indexOf('await applyTelemetry');
  const syncAt = deviceExchange.indexOf('await buildDeviceSyncPayloadForDevice');
  assert.ok(mergeAt >= 0);
  assert.ok(syncAt > mergeAt);
  assert.match(deviceExchange, /queueSchedulerWatchdog\(env, ctx\)/);
});

test('Octopus uses a daily-only, gap-safe cursor-bounded D1 model', () => {
  assert.match(octopusScheduleMigration, /PRIMARY KEY\(account_number, day\)/);
  assert.match(octopusScheduleMigration, /WITHOUT ROWID/);
  assert.match(octopusScheduleMigration, /DROP TABLE IF EXISTS octopus_readings/);
  assert.match(octopusScheduleMigration, /CREATE TABLE octopus_sync_state/);
  assert.match(octopusHistory, /OCTOPUS_CORRECTION_OVERLAP_DAYS = 1/);
  assert.match(octopusHistory, /json_each\(\?2\)/);
  assert.match(octopusHistory, /contiguousStoredThrough/);
  assert.match(octopusHistory, /ON CONFLICT\(account_number,day\)/);
  assert.match(octopusHistory, /SELECT day,energy_kwh,slot_count/);
  assert.doesNotMatch(octopusHistory, /INSERT INTO octopus_readings/);
  assert.doesNotMatch(octopusHistory, /DELETE FROM octopus_readings/);
});

test('high-frequency D1 tables and legacy telemetry are compacted', () => {
  assert.match(runtimeMigration, /CREATE TABLE jobs_v2[\s\S]*WITHOUT ROWID/);
  assert.match(runtimeMigration, /CREATE TABLE current_state_v2[\s\S]*WITHOUT ROWID/);
  assert.match(runtimeMigration, /CREATE TABLE device_heartbeats_v2[\s\S]*WITHOUT ROWID/);
  assert.match(runtimeMigration, /CREATE TABLE sync_manifest/);
  assert.match(runtimeMigration, /CREATE TABLE job_events/);
  assert.match(runtimeMigration, /DROP TABLE IF EXISTS environment_samples/);
  assert.match(runtimeMigration, /DROP TABLE IF EXISTS environment_buckets/);
  assert.match(runtimeBugfixMigration, /WHEN NEW\.source IN/);
  assert.match(runtimeBugfixMigration, /BEFORE DELETE ON videos/);
  assert.match(runtimeBugfixMigration, /AFTER DELETE ON ranking_entries/);
  assert.match(telemetryHeartbeat, /HEARTBEAT_REFRESH_MS = 6 \* 60 \* 60_000/);
  assert.match(telemetryHeartbeat, /last_seen_at<=\?7/);
});

test('modeled daily D1 written rows stay below the 3000-row target', () => {
  const schedulerCompletionRows = 0;
  const switchbotChangedStateReserve = 24;
  const heartbeatStateRows = heartbeatStateIntervals
    .reduce((total, interval) => total + throttledHeartbeatWrites(interval), 0);
  const compactTelemetryHeartbeatRows = runsPerDay(6 * 60 * 60);
  const livenessStateRows = runsPerDay(scheduledIntervals.video_liveness);
  const jobFailureRecoveryReserve = 4;
  const octopusDailyAndCursorRows = 3;
  const changeCommandWebhookAndVideoMutationReserve = 1_500;
  const modeledRows = schedulerCompletionRows
    + switchbotChangedStateReserve
    + heartbeatStateRows
    + compactTelemetryHeartbeatRows
    + livenessStateRows
    + jobFailureRecoveryReserve
    + octopusDailyAndCursorRows
    + changeCommandWebhookAndVideoMutationReserve;

  assert.equal(heartbeatStateRows, 17);
  assert.equal(compactTelemetryHeartbeatRows, 4);
  assert.equal(livenessStateRows, 24);
  assert.equal(modeledRows, 1_576);
  assert.ok(modeledRows < TARGET);
});

test('modeled daily Worker invocations stay below the 3000-request target', () => {
  const nativeExchangeRequests = runsPerDay(1800);
  const schedulerAlarmInvocations = modeledSchedulerAlarms();
  // Count one internal DO ensure for every native exchange as a deliberately
  // conservative upper bound; isolate-local throttling normally makes it lower.
  const schedulerEnsureSignals = nativeExchangeRequests;
  const radarGenerationReserve = 50;
  const apiWebhookVideoReserve = 1_900;
  const modeledRequests = nativeExchangeRequests
    + schedulerAlarmInvocations
    + schedulerEnsureSignals
    + radarGenerationReserve
    + apiWebhookVideoReserve;

  assert.equal(nativeExchangeRequests, 48);
  assert.equal(schedulerAlarmInvocations, 148);
  assert.equal(modeledRequests, 2_194);
  assert.ok(modeledRequests < TARGET);
});

test('bounded R2 writes remain modest', () => {
  const telemetryStateWrites = runsPerDay(120 * 60);
  const radarLatestReserve = 24;
  assert.equal(telemetryStateWrites + radarLatestReserve, 36);
});
