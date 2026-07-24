import assert from 'node:assert/strict';
import test from 'node:test';

import {
  syncCompactedFeedInDatabase
} from '../src/compacted-feed-sql.js';
import { recentFeedCutoff } from '../src/source-feed-time.js';

function createDb() {
  const db = {
    statements: [],
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
      db.statements = statements;
      return [
        { results: [], meta: { changes: 1 } },
        { results: [], meta: { changes: 2 } },
        { results: [], meta: { changes: 3 } },
        {
          results: [{ rowCount: 2, contentJson: '["9","4"]' }],
          meta: { changes: 0 }
        }
      ];
    }
  };
  return db;
}

test('compacted feed synchronization stays inside four D1 statements', async () => {
  const db = createDb();
  const capturedAt = '2026-07-18T22:00:00.000Z';
  const result = await syncCompactedFeedInDatabase(db, capturedAt);

  assert.deepEqual(result, { rowCount: 2, contentJson: '["9","4"]' });
  assert.equal(db.statements.length, 4);

  const [removeStale, parkMoved, upsertDesired, signature] = db.statements;
  for (const statement of [removeStale, parkMoved, upsertDesired]) {
    assert.match(statement.sql, /^WITH desired AS/);
    assert.match(statement.sql, /ROW_NUMBER\(\) OVER/);
    assert.match(statement.sql, /video\.last_seen_at DESC, video\.id DESC/);
    assert.match(statement.sql, /video\.status = 'active'/);
    assert.doesNotMatch(statement.sql, /video_blocklist|video_death_list/);
    assert.match(statement.sql, /LIMIT \?/);
    assert.doesNotMatch(statement.sql, /OFFSET/);
  }

  assert.match(removeStale.sql, /DELETE FROM ranking_entries/);
  assert.match(parkMoved.sql, /SET rank = -video_id/);
  assert.match(upsertDesired.sql, /INSERT INTO ranking_entries/);
  assert.match(upsertDesired.sql, /ON CONFLICT\(period, video_id\) DO UPDATE/);
  assert.match(signature.sql, /json_group_array\(CAST\(video_id AS TEXT\)\)/);
  assert.match(signature.sql, /ORDER BY rank/);

  const cutoff = recentFeedCutoff(capturedAt);
  assert.deepEqual(removeStale.args, [cutoff, 2000, '24h']);
  assert.deepEqual(parkMoved.args, [cutoff, 2000, '24h']);
  assert.deepEqual(upsertDesired.args, [cutoff, 2000, '24h', capturedAt]);
  assert.deepEqual(signature.args, ['24h']);
});
