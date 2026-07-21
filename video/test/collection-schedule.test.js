import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';

import {
  LIVENESS_INTERVAL_SECONDS,
  LIVENESS_JOB_NAME,
  LIVENESS_SCHEDULE
} from '../src/liveness-schedule.js';
import { MANUAL_IMPORT_QUEUE_NAME } from '../src/manual-import-queue.js';

const wrangler = JSON.parse(await readFile(new URL('../wrangler.jsonc', import.meta.url), 'utf8'));
const unifiedWrangler = JSON.parse(await readFile(
  new URL('../../cloud/wrangler.jsonc', import.meta.url),
  'utf8'
));
const entryCore = await readFile(new URL('../src/entry-core.js', import.meta.url), 'utf8');

test('deployment has no video cron and unified HomePanel owns manual import queues', () => {
  assert.equal(wrangler.triggers, undefined);
  assert.equal(unifiedWrangler.triggers, undefined);
  assert.equal(LIVENESS_JOB_NAME, 'video_liveness');
  assert.equal(LIVENESS_INTERVAL_SECONDS, 12 * 60);
  assert.equal(LIVENESS_SCHEDULE, 'homepanel-alarm:720s');
  assert.equal(wrangler.queues, undefined);
  assert.deepEqual(unifiedWrangler.queues?.producers, [{
    binding: 'MANUAL_IMPORT_QUEUE',
    queue: MANUAL_IMPORT_QUEUE_NAME
  }]);
  assert.deepEqual(unifiedWrangler.queues?.consumers, [{
    queue: MANUAL_IMPORT_QUEUE_NAME,
    max_batch_size: 1,
    max_batch_timeout: 0,
    max_retries: 5,
    retry_delay: 300,
    max_concurrency: 1,
    dead_letter_queue: 'videoscraper-manual-imports-dlq'
  }]);
});

test('automatic source collection remains explicitly disabled', () => {
  assert.match(entryCore, /async queue\(batch, env\)/);
  assert.doesNotMatch(entryCore, /MANUAL_IMPORT_CRON/);
  assert.match(entryCore, /scheduled-collection-disabled/);
  assert.doesNotMatch(entryCore, /runScheduledCollectionGroup\(/);
});

test('manual collect-all remains the explicit collection entry point', () => {
  assert.match(entryCore, /(?:url\.)?pathname === '\/api\/admin\/collect-all'/);
  assert.match(entryCore, /runAllScheduledCollections\(env\)/);
});
