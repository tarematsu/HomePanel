import assert from 'node:assert/strict';
import test from 'node:test';

import { blockPlaybackMedia } from '../src/video-blocklist.js';

function createDb(state) {
  const db = {
    lookup: null,
    batchStatements: [],
    prepare(sql) {
      const statement = {
        sql: sql.replace(/\s+/g, ' ').trim(),
        args: [],
        bind(...args) {
          this.args = args;
          return this;
        },
        async first() {
          db.lookup = { sql: this.sql, args: this.args };
          return state;
        }
      };
      return statement;
    },
    async batch(statements) {
      db.batchStatements = statements.map((statement) => ({
        sql: statement.sql,
        args: statement.args
      }));
      return [
        { meta: { changes: 1 } },
        { meta: { changes: 1 } },
        { meta: { changes: 0 } }
      ];
    }
  };
  return db;
}

test('NG registration accepts an active video outside ranking_entries', async () => {
  const mediaUrl = 'https://video.twimg.com/ext_tw_video/123/pu/vid/720x1280/example.mp4?tag=12';
  const canonicalKey = 'video.twimg.com/ext_tw_video/123/pu/vid/720x1280/example.mp4';
  const db = createDb({
    alreadyBlocked: 0,
    id: 42,
    mediaUrl,
    canonicalKey
  });
  const request = new Request('https://app.example/api/videos/block', {
    method: 'POST',
    headers: {
      authorization: 'Bearer secret',
      'content-type': 'application/json'
    },
    body: JSON.stringify({ mediaUrl })
  });

  const result = await blockPlaybackMedia({
    ADMIN_TOKEN: 'secret',
    MEDIA_HOST: 'video.twimg.com',
    DB: db
  }, request);

  assert.equal(result.status, 200);
  assert.equal(result.data.ok, true);
  assert.equal(result.data.blocked, true);
  assert.equal(result.data.videoId, 42);
  assert.deepEqual(db.lookup.args, [canonicalKey, canonicalKey]);
  assert.doesNotMatch(db.lookup.sql, /JOIN ranking_entries/);
  assert.match(db.lookup.sql, /video\.status = 'active'/);
  assert.doesNotMatch(db.lookup.sql, /video_death_list/);
  assert.equal(db.batchStatements.length, 3);
  assert.match(db.batchStatements[0].sql, /INSERT INTO video_blocklist/);
  assert.match(db.batchStatements[1].sql, /UPDATE videos/);
  assert.match(db.batchStatements[2].sql, /DELETE FROM ranking_entries/);
});
