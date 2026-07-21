import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';

const entry = await readFile(new URL('../src/entry.js', import.meta.url), 'utf8');
const entryCore = await readFile(new URL('../src/entry-core.js', import.meta.url), 'utf8');
const compactedFeed = await readFile(
  new URL('../src/source-feed-compacted.js', import.meta.url),
  'utf8'
);
const playbackFeedSync = await readFile(
  new URL('../src/playback-feed-sync.js', import.meta.url),
  'utf8'
);
const worker = await readFile(new URL('../src/worker.js', import.meta.url), 'utf8');

test('authenticated status responses bypass shared edge cache and remain private', () => {
  assert.match(entryCore, /if \(!authorized\(request, env\)\) return unauthorized\(\)/);
  assert.match(entry, /return core\.fetch\(request, env, ctx\)/);
  assert.match(entry, /migrationFreezeEnabled/);
  assert.doesNotMatch(entry, /protectPrivateStatusResponse/);
  assert.doesNotMatch(entry, /cache\.match\(/);
  assert.doesNotMatch(entry, /cache\.put\(/);
  assert.match(entryCore, /'cache-control', 'private, no-store'/);
  assert.doesNotMatch(entryCore, /STATUS_SHARED_CACHE_CONTROL/);
});

test('admin collect-all runs the active source set once with one feed finalization', () => {
  assert.match(entryCore, /runAllScheduledCollections\(env\)/);
  assert.doesNotMatch(entryCore, /for \(const path of ADMIN_COLLECTION_PATHS\)/);
});

test('individual admin collectors return accepted and finalize one compacted feed', () => {
  assert.match(entryCore, /ADMIN_COLLECTION_PATHS\.includes\((?:url\.)?pathname\)/);
  assert.match(entryCore, /runOneAdminCollector\((?:url\.)?pathname, env, ctx\)/);
  assert.match(entryCore, /finally\(\(\) => invalidateCaches\(env\.DB\)\)/);
  assert.match(entryCore, /status: 202/);
  assert.match(worker, /deferFeedMaintenance: true/);
  assert.match(worker, /finalizeCompactedFeed\(env\)/);
  assert.doesNotMatch(worker, /json\(await runAndRecord/);
});

test('compacted finalization performs set synchronization in D1 under the shared feed lock', () => {
  assert.match(compactedFeed, /withPlaybackFeedFinalization\(db, async \(\) =>/);
  assert.match(compactedFeed, /syncCompactedFeedInDatabase\(db, capturedAt\)/);
  assert.match(compactedFeed, /serializedFeedContentHash\(synchronized\.contentJson\)/);
  assert.doesNotMatch(compactedFeed, /desiredFeedStatement/);
  assert.doesNotMatch(compactedFeed, /syncCompactedFeedRows/);
  assert.doesNotMatch(compactedFeed, /rebuildPlaybackFeed\(db, capturedAt\)/);
});

test('playback feed rebuild hashes and commits rows through the shared feed lock', () => {
  assert.match(playbackFeedSync, /return \{ count: plan\.desiredCount, rows: desiredRows \}/);
  assert.match(playbackFeedSync, /withPlaybackFeedFinalization\(db, async \(\) =>/);
  assert.match(playbackFeedSync, /feedContentHash\(rows\)/);
  assert.doesNotMatch(playbackFeedSync, /writeFeedState\(db, hash, count, capturedAt\)/);
});
