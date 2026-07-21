import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';

const cloudConfig = JSON.parse(readFileSync(new URL('../../cloud/wrangler.jsonc', import.meta.url), 'utf8'));

const DAY_SECONDS = 86_400;
const TARGET = 5_000;
const STATE_HEARTBEAT_SECONDS = 30 * 60;
const scheduledIntervals = {
  switchbot: 300,
  stationhead: 300,
  stationhead_health: 300,
  news: 600,
  weather: 3_600,
  octopus: 21_600,
  video_liveness: 720,
  update_check: 1_800,
  cleanup: 86_400
};
const stateIntervals = [300, 300, 300, 600, 3_600, 21_600];

function runsPerDay(intervalSeconds) {
  return Math.ceil(DAY_SECONDS / intervalSeconds);
}

function throttledHeartbeatWrites(intervalSeconds) {
  const effectiveInterval = Math.ceil(STATE_HEARTBEAT_SECONDS / intervalSeconds) * intervalSeconds;
  return runsPerDay(effectiveInterval);
}

test('static assets bypass the Worker while dynamic routes remain Worker-first', () => {
  assert.deepEqual(cloudConfig.assets.run_worker_first, ['/api/*', '/v1/*', '/admin']);
  assert.notEqual(cloudConfig.assets.run_worker_first, true);
});

test('Workers KV is absent while the bounded R2 data cache remains declared', () => {
  assert.equal(cloudConfig.kv_namespaces, undefined);
  assert.ok(cloudConfig.r2_buckets.some((entry) => entry.binding === 'DATA_BUCKET'));
});

test('modeled daily D1 writes stay below the 5000-row target without KV', () => {
  const schedulerCompletionWrites = Object.values(scheduledIntervals)
    .reduce((total, interval) => total + runsPerDay(interval), 0);
  assert.equal(schedulerCompletionWrites, 1_205);

  const stateHeartbeatWrites = stateIntervals
    .reduce((total, interval) => total + throttledHeartbeatWrites(interval), 0);
  const telemetryWrites = 48 * 4;
  const changeAndControlReserve = 3_000;
  const modeledWrites = schedulerCompletionWrites
    + stateHeartbeatWrites
    + telemetryWrites
    + changeAndControlReserve;

  assert.equal(stateHeartbeatWrites, 220);
  assert.equal(modeledWrites, 4_617);
  assert.ok(modeledWrites < TARGET);
});

test('modeled daily Worker invocations stay below the 5000-request target', () => {
  const nativeSyncRequests = runsPerDay(300);
  const schedulerAlarmInvocations = Object.values(scheduledIntervals)
    .reduce((total, interval) => total + runsPerDay(interval), 0);
  const radarMissReserve = 100;
  const apiAndVideoReserve = 3_000;
  const modeledRequests = nativeSyncRequests
    + schedulerAlarmInvocations
    + radarMissReserve
    + apiAndVideoReserve;

  assert.equal(modeledRequests, 4_593);
  assert.ok(modeledRequests < TARGET);
});
