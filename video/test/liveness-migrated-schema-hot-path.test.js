import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';

const liveness = await readFile(new URL('../src/liveness-monitor.js', import.meta.url), 'utf8');
const lists = await readFile(new URL('../src/status-lists.js', import.meta.url), 'utf8');

test('liveness hot paths rely directly on migrated schema without D1 leases', () => {
  assert.doesNotMatch(liveness, /ensureDatabaseOnce/);
  assert.doesNotMatch(liveness, /ensureDbIndexes/);
  assert.doesNotMatch(liveness, /ensureVideoBlocklistTable/);
  assert.doesNotMatch(liveness, /ensureVideoDeathListTable/);
  assert.doesNotMatch(liveness, /ensureStateTable/);
  assert.doesNotMatch(liveness, /ensureLivenessStateTable/);
  assert.doesNotMatch(liveness, /CREATE TABLE IF NOT EXISTS video_liveness_state/);
  assert.doesNotMatch(liveness, /INSERT OR IGNORE INTO video_liveness_state/);
  assert.doesNotMatch(liveness, /SET lock_token =/);
  assert.match(liveness, /export async function runLivenessMonitor\(env\) \{\s+const initialState = await readRunState/);
});

test('status snapshots do not await a migrated schema no-op', () => {
  assert.doesNotMatch(lists, /ensureLivenessStateTable/);
  assert.match(lists, /export async function readStatusSnapshot\(db, limit\) \{\s+const \[state/);
});
