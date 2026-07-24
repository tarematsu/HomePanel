import assert from 'node:assert/strict';
import test from 'node:test';

import {
  applyProbeResults,
  buildLivenessStatus,
  classifyProbeStatus,
  LIVENESS_BATCH_SIZE,
  probeVideoUrl,
  recordRunAndRelease,
  shouldRefreshAliveMetadata
} from '../src/liveness-monitor.js';
import { LIVENESS_CRON } from '../src/liveness-schedule.js';

function createMutationDb() {
  const batches = [];
  return {
    batches,
    prepare(sql) {
      return {
        sql: sql.replace(/\s+/g, ' ').trim(),
        args: [],
        bind(...args) {
          this.args = args;
          return this;
        }
      };
    },
    async batch(statements) {
      batches.push(statements);
      return statements.map(() => ({ results: [], meta: { changes: 0 } }));
    }
  };
}

function createRunRecordDb(changes) {
  const statements = [];
  return {
    statements,
    prepare(sql) {
      return {
        sql: sql.replace(/\s+/g, ' ').trim(),
        args: [],
        bind(...args) {
          this.args = args;
          return this;
        },
        async run() {
          statements.push(this);
          return { meta: { changes } };
        }
      };
    }
  };
}

function probeRows(count) {
  return Array.from({ length: count }, (_, index) => ({
    id: index + 1,
    mediaUrl: `https://cdn.example/${index + 1}.mp4`,
    canonicalKey: `video-${index + 1}`,
    lastHttpStatus: 206,
    failCount: 0
  }));
}

test('liveness preserves 120 daily checks as five-probe hourly invocations', () => {
  assert.equal(LIVENESS_CRON, '0 * * * *');
  assert.equal(LIVENESS_BATCH_SIZE, 5);
  assert.equal(24 * LIVENESS_BATCH_SIZE, 120);
});

test('liveness status is built from the shared state row and list count', () => {
  const status = buildLivenessStatus({
    phase: 'death',
    deathCursorKey: 'video-b',
    deathUpperKey: 'video-z',
    cycle: 4,
    checkedTotal: 250,
    deadTotal: 9,
    revivedTotal: 2,
    lastCheckedCount: 5
  }, 7);

  assert.equal(status.count, 7);
  assert.equal(status.phase, 'death');
  assert.equal(status.cursor, 'video-b');
  assert.equal(status.upperBound, 'video-z');
  assert.equal(status.checkedTotal, 250);
  assert.equal(status.lastCheckedCount, 5);
  assert.equal(status.batchSize, 5);
  assert.equal(status.concurrency, 5);
});

test('only definitive missing responses are classified as dead', () => {
  assert.equal(classifyProbeStatus(206), 'alive');
  assert.equal(classifyProbeStatus(302), 'alive');
  assert.equal(classifyProbeStatus(416), 'alive');
  assert.equal(classifyProbeStatus(404), 'dead');
  assert.equal(classifyProbeStatus(410), 'dead');
  assert.equal(classifyProbeStatus(429), 'unknown');
  assert.equal(classifyProbeStatus(503), 'unknown');
});

test('unchanged healthy probes do not require a video-row write', () => {
  assert.equal(shouldRefreshAliveMetadata({ lastHttpStatus: 206, failCount: 0 }, 206), false);
  assert.equal(shouldRefreshAliveMetadata({ lastHttpStatus: 200, failCount: 0 }, 206), true);
  assert.equal(shouldRefreshAliveMetadata({ lastHttpStatus: 206, failCount: 2 }, 206), true);
});

test('base dead probes rely on schema triggers for counts', async () => {
  const db = createMutationDb();
  const rows = probeRows(5);
  const probes = rows.map(() => ({ state: 'dead', status: 404 }));
  const result = await applyProbeResults(
    db, 'base', rows, probes, '2026-07-02T00:00:00.000Z'
  );

  assert.equal(result.deadCount, 5);
  assert.equal(result.feedChanged, true);
  assert.equal(db.batches.length, 1);
  assert.equal(db.batches[0].length, 3);
  assert.equal(db.batches[0][0].sql.includes('INSERT INTO video_death_list'), true);
  assert.equal(db.batches[0][0].sql.includes('COALESCE(video_death_list.check_count, 0) + 1'), true);
  assert.equal(db.batches[0][1].sql.includes('UPDATE videos'), true);
  assert.equal(db.batches[0][1].sql.includes('last_http_status = COALESCE(input.httpStatus, videos.last_http_status)'), true);
  assert.equal(db.batches[0][2].sql.startsWith('DELETE FROM ranking_entries'), true);
  assert.equal(db.batches[0].some(statement => statement.sql.includes('UPDATE status_counts')), false);
});

test('unchanged alive base probes perform no D1 mutation batch', async () => {
  const db = createMutationDb();
  const rows = probeRows(5);
  const probes = rows.map(() => ({ state: 'alive', status: 206 }));
  const result = await applyProbeResults(
    db, 'base', rows, probes, '2026-07-02T00:00:00.000Z'
  );

  assert.equal(result.deadCount, 0);
  assert.equal(result.unknownCount, 0);
  assert.equal(result.feedChanged, false);
  assert.equal(db.batches.length, 0);
});

test('unknown base probes preserve the previous definitive HTTP status', async () => {
  const db = createMutationDb();
  const rows = [{ ...probeRows(1)[0], failCount: 1, lastHttpStatus: 206 }];
  const probes = [{ state: 'unknown', status: null }];
  const result = await applyProbeResults(
    db, 'base', rows, probes, '2026-07-02T00:00:00.000Z'
  );

  assert.equal(result.unknownCount, 1);
  assert.equal(db.batches.length, 1);
  assert.equal(db.batches[0].length, 1);
  assert.equal(db.batches[0][0].sql.includes('last_http_status = COALESCE(input.httpStatus, videos.last_http_status)'), true);
});

test('death checks restore revived ranks and rely on triggers for counts', async () => {
  const db = createMutationDb();
  const rows = probeRows(3);
  const probes = [
    { state: 'alive', status: 206 },
    { state: 'unknown', status: 503 },
    { state: 'dead', status: 404 }
  ];
  const result = await applyProbeResults(
    db, 'death', rows, probes, '2026-07-02T00:00:00.000Z'
  );

  assert.equal(result.revivedCount, 1);
  assert.equal(result.unknownCount, 1);
  assert.equal(result.feedChanged, true);
  assert.equal(db.batches.length, 1);
  assert.equal(db.batches[0].length, 4);
  assert.equal(db.batches[0][0].sql.startsWith('DELETE FROM video_death_list'), true);
  assert.equal(db.batches[0][1].sql.includes('UPDATE videos'), true);
  assert.equal(db.batches[0][2].sql.startsWith('INSERT OR IGNORE INTO ranking_entries'), true);
  assert.equal(db.batches[0][3].sql.includes('UPDATE video_death_list'), true);
  assert.equal(db.batches[0].some(statement => statement.sql.includes('UPDATE status_counts')), false);
});

test('run recording reports whether the singleton state was updated without a lease predicate', async () => {
  const successDb = createRunRecordDb(1);
  const missingDb = createRunRecordDb(0);
  const values = {
    checkedCount: 3,
    deadCount: 1,
    revivedCount: 0,
    unknownCount: 2,
    completedAt: '2026-07-02T00:00:00.000Z',
    error: null
  };
  const state = {
    phase: 'base',
    baseCursorId: 5,
    baseUpperId: 10,
    deathCursorKey: '',
    deathUpperKey: '',
    cycle: 1
  };

  assert.equal(await recordRunAndRelease(successDb, null, values, state), true);
  assert.equal(await recordRunAndRelease(missingDb, null, values, state), false);
  assert.equal(successDb.statements[0].sql.includes('WHERE id = 1'), true);
  assert.equal(successDb.statements[0].sql.includes('lock_token = ?'), false);
  assert.equal(successDb.statements[0].args.includes('token-a'), false);
});

test('run recording preserves totals when old rows contain NULL counters', async () => {
  const db = createRunRecordDb(1);
  const values = {
    checkedCount: 3,
    deadCount: 1,
    revivedCount: 1,
    unknownCount: 1,
    completedAt: '2026-07-02T00:00:00.000Z',
    error: null
  };

  assert.equal(await recordRunAndRelease(db, null, values, null), true);
  assert.equal(db.statements[0].sql.includes('checked_total = COALESCE(checked_total, 0) + ?'), true);
  assert.equal(db.statements[0].sql.includes('dead_total = COALESCE(dead_total, 0) + ?'), true);
  assert.equal(db.statements[0].sql.includes('revived_total = COALESCE(revived_total, 0) + ?'), true);
});

test('probe requests only the first byte and cancels the response body', async () => {
  let request;
  let cancelled = false;
  const result = await probeVideoUrl('https://cdn.example/example.mp4', async (_url, init) => {
    request = init;
    return {
      status: 206,
      body: {
        async cancel() {
          cancelled = true;
        }
      }
    };
  });

  assert.equal(result.state, 'alive');
  assert.equal(request.method, 'GET');
  assert.equal(request.headers.range, 'bytes=0-0');
  assert.equal(cancelled, true);
});
