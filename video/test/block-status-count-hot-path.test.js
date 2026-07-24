import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';

const source = await readFile(new URL('../src/video-blocklist.js', import.meta.url), 'utf8');

test('successful block avoids full recount while preserving standalone status updates', () => {
  assert.doesNotMatch(source, /refreshStatusCounts/);
  assert.doesNotMatch(source, /playback-exclusion-status-refresh-failed/);
  assert.match(source, /const results = await env\.DB\.batch\(\[/);
  assert.match(source, /INSERT INTO video_blocklist/);
  assert.match(source, /UPDATE videos/);
  assert.match(source, /DELETE FROM ranking_entries/);
  assert.match(source, /statusCountsRefreshed: true/);
});
