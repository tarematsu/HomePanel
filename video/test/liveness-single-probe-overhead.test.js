import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';

const source = await readFile(new URL('../src/liveness-monitor.js', import.meta.url), 'utf8');

test('hourly liveness reads state without acquiring a D1 lease', () => {
  assert.match(source, /async function readRunState\(db\)[\s\S]*prepareLivenessStateRead\(db\)\.first\(\)/);
  assert.doesNotMatch(source, /SET lock_token = \?/);
  assert.doesNotMatch(source, /acquireRunState/);
  assert.match(source, /const initialState = await readRunState\(env\.DB\)/);
});

test('phase transitions initialize bounds with read-only snapshot queries', () => {
  assert.match(source, /SELECT \$\{DEATH_UPPER_KEY_SQL\} AS deathUpperKey/);
  assert.match(source, /SELECT \$\{BASE_UPPER_ID_SQL\} AS baseUpperId/);
  assert.doesNotMatch(source, /SET phase = 'death'/);
  assert.doesNotMatch(source, /SET phase = 'base'/);
});

test('five-video liveness probes concurrently and advances only after mutations succeed', () => {
  assert.doesNotMatch(source, /mapWithConcurrency/);
  assert.doesNotMatch(source, /getLivenessStatus/);
  assert.doesNotMatch(source, /SELECT COUNT\(\*\) AS count FROM video_death_list/);

  const run = source.slice(source.indexOf('export async function runLivenessMonitor'));
  assert.match(run, /Promise\.all\(rows\.map\(row => probeVideoUrl\(row\.mediaUrl\)\)\)/);
  assert.match(run, /const phase = state\.phase/);
  assert.match(run, /applyProbeResults\(env\.DB, phase, rows, probes/);
  assert.ok(run.indexOf('await applyProbeResults') < run.indexOf('state = { ...state, baseCursorId'));
  assert.equal((run.match(/recordRunAndRelease\(/g) || []).length, 1);
});
