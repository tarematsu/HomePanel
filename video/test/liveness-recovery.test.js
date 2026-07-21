import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';

const originalMigration = readFileSync(
  new URL('../migrations/903_liveness_feed_dirty.sql', import.meta.url),
  'utf8'
);
const retirementMigration = readFileSync(
  new URL('../migrations/100004_retire_liveness_feed_dirty.sql', import.meta.url),
  'utf8'
);
const entrySource = readFileSync(
  new URL('../src/entry.js', import.meta.url),
  'utf8'
);


test('D1 preserves liveness progress whenever a run is recorded with an error', () => {
  assert.match(originalMigration, /ADD COLUMN feed_dirty INTEGER NOT NULL DEFAULT 0/i);
  assert.match(originalMigration, /AFTER UPDATE OF last_error ON video_liveness_state/i);
  assert.match(originalMigration, /NEW\.last_error IS NOT NULL/i);
  for (const column of [
    'phase',
    'base_cursor_id',
    'base_upper_id',
    'death_cursor_key',
    'death_upper_key',
    'cycle'
  ]) {
    assert.match(originalMigration, new RegExp(`${column} = OLD\\.${column}`, 'i'));
  }
});

test('obsolete feed-dirty triggers are retired without removing error recovery', () => {
  assert.match(retirementMigration, /DROP TRIGGER IF EXISTS liveness_feed_dirty_on_death_insert/i);
  assert.match(retirementMigration, /DROP TRIGGER IF EXISTS liveness_feed_dirty_on_death_delete/i);
  assert.doesNotMatch(retirementMigration, /liveness_preserve_progress_on_error/i);
});

test('scheduled liveness delegates directly without a post-task D1 acknowledgement', () => {
  assert.doesNotMatch(entrySource, /liveness-feed-repair/);
  assert.doesNotMatch(entrySource, /createLivenessRepairContext/);
  assert.match(entrySource, /return core\.scheduled\(controller, env, ctx\)/);
  assert.match(entrySource, /migrationFreezeEnabled/);
  assert.doesNotMatch(entrySource, /liveness.*acknowledg/i);
});
