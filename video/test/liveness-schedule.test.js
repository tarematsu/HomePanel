import assert from 'node:assert/strict';
import test from 'node:test';

import { LIVENESS_BATCH_SIZE } from '../src/liveness-monitor.js';
import {
  LIVENESS_INTERVAL_SECONDS,
  LIVENESS_JOB_NAME,
  LIVENESS_SCHEDULE
} from '../src/liveness-schedule.js';

test('liveness preserves 120 daily probes as five probes per hourly alarm', () => {
  assert.equal(LIVENESS_JOB_NAME, 'video_liveness');
  assert.equal(LIVENESS_INTERVAL_SECONDS, 60 * 60);
  assert.equal(LIVENESS_SCHEDULE, 'homepanel-alarm:3600s');
  assert.equal(LIVENESS_BATCH_SIZE, 5);
  assert.equal((24 * 60 * 60) / LIVENESS_INTERVAL_SECONDS * LIVENESS_BATCH_SIZE, 120);
});
