import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';

import {
  buildWorkerCpuMarkdown,
  microsecondsToMilliseconds,
  summarizeWorkerCpuRows
} from '../scripts/report-worker-cpu.mjs';

test('CPU microseconds are converted to milliseconds', () => {
  assert.equal(microsecondsToMilliseconds(10_000), 10);
  assert.equal(microsecondsToMilliseconds(250), 0.25);
});

test('CPU summary selects maximum percentile buckets without inventing a global percentile', () => {
  const rows = [
    {
      dimensions: { datetime: '2026-07-19T10:00:00Z', status: 'success' },
      sum: { requests: 4, errors: 0, subrequests: 8 },
      quantiles: { cpuTimeP50: 1_500, cpuTimeP99: 9_500 }
    },
    {
      dimensions: { datetime: '2026-07-19T10:01:00Z', status: 'success' },
      sum: { requests: 6, errors: 1, subrequests: 10 },
      quantiles: { cpuTimeP50: 2_000, cpuTimeP99: 12_500 }
    }
  ];

  const summary = summarizeWorkerCpuRows(rows, { targetMs: 10 });
  assert.equal(summary.rowCount, 2);
  assert.deepEqual(summary.totals, { requests: 10, errors: 1, subrequests: 18 });
  assert.equal(summary.maxBucketP50.milliseconds, 2);
  assert.equal(summary.maxBucketP99.milliseconds, 12.5);
  assert.equal(summary.maxBucketP99.datetime, '2026-07-19T10:01:00Z');
  assert.equal(summary.maxBucketP99AtOrBelowTarget, false);
});

test('empty analytics data is explicit and does not claim the target was met', () => {
  const summary = summarizeWorkerCpuRows([], { targetMs: 10 });
  assert.equal(summary.hasData, false);
  assert.equal(summary.maxBucketP99, null);
  assert.equal(summary.maxBucketP99AtOrBelowTarget, null);

  const markdown = buildWorkerCpuMarkdown({
    workerName: 'videoscraper',
    datetimeStart: '2026-07-19T00:00:00.000Z',
    datetimeEnd: '2026-07-19T06:00:00.000Z',
    summary
  });
  assert.match(markdown, /Maximum bucket P99: n\/a/);
  assert.match(markdown, /Maximum bucket P99 <= target: no data/);
  assert.match(markdown, /does not prove that every route/);
});

test('CPU diagnostics workflow is manual-only', async () => {
  const workflow = await readFile(
    new URL('../../.github/workflows/video-worker-cpu-report.yml', import.meta.url),
    'utf8'
  );
  assert.match(workflow, /^\s*workflow_dispatch:/m);
  assert.doesNotMatch(workflow, /^\s*schedule:/m);
});
