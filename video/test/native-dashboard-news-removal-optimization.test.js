import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';

const dashboardParser = readFileSync(
  new URL('../../native/src/dashboard_data.cpp', import.meta.url),
  'utf8',
);
const dashboardDataHeader = readFileSync(
  new URL('../../native/src/dashboard_data.h', import.meta.url),
  'utf8',
);
const dashboardLoader = readFileSync(
  new URL('../../native/src/renderer_dashboard.cpp', import.meta.url),
  'utf8',
);
const nativePlayback = readFileSync(
  new URL('../../native/src/dashboard_native_playback.cpp', import.meta.url),
  'utf8',
);
const playbackResolve = readFileSync(
  new URL('../../native/src/dashboard_playback_resolve.cpp', import.meta.url),
  'utf8',
);
const artworkCache = readFileSync(
  new URL('../../native/src/artwork_cache.h', import.meta.url),
  'utf8',
);
const airHistory = readFileSync(
  new URL('../../native/src/app_air_history.cpp', import.meta.url),
  'utf8',
);
const loggerHeader = readFileSync(
  new URL('../../native/src/logger.h', import.meta.url),
  'utf8',
);
const loggerSource = readFileSync(
  new URL('../../native/src/logger.cpp', import.meta.url),
  'utf8',
);
const radarUi = readFileSync(
  new URL('../../native/src/renderer_radar_ui.cpp', import.meta.url),
  'utf8',
);
const bitmapCache = readFileSync(
  new URL('../../native/src/renderer_bitmap_cache.cpp', import.meta.url),
  'utf8',
);
const rendererHeader = readFileSync(
  new URL('../../native/src/web_renderer.h', import.meta.url),
  'utf8',
);
const panelState = readFileSync(
  new URL('../../native/src/renderer_panel_state.cpp', import.meta.url),
  'utf8',
);
const panelWindows = readFileSync(
  new URL('../../native/src/renderer_panels/windows.inc', import.meta.url),
  'utf8',
);
const dataSections = readFileSync(
  new URL('../../native/src/renderer_panels/data_sections.inc', import.meta.url),
  'utf8',
);
const layout = readFileSync(
  new URL('../../native/src/renderer_panels/layout_overrides.inc', import.meta.url),
  'utf8',
);

test('removed native News panel no longer parses or hashes News payloads', () => {
  assert.doesNotMatch(dashboardParser, /json::Object\(root, L"news"\)/);
  assert.doesNotMatch(dashboardParser, /newsItems\.reserve|next\.newsItems\.push_back/);
  assert.doesNotMatch(dashboardParser, /next\.revisions\.news/);
});

test('News data types and snapshot storage are removed', () => {
  assert.doesNotMatch(dashboardDataHeader, /NewsItemData/);
  assert.doesNotMatch(dashboardDataHeader, /newsItems|newsItemCount/);
  assert.doesNotMatch(dashboardDataHeader, /uint64_t news/);
});

test('Renderer retains no News enum, drawing declaration, or state member', () => {
  assert.doesNotMatch(rendererHeader, /PanelSection::News|\bNews\s*[,}]/);
  assert.doesNotMatch(rendererHeader, /DrawNewsSection/);
  assert.doesNotMatch(rendererHeader, /nativeNewsIndex_|nativeNewsRenderRevision_|newsCount_/);
});

test('dashboard loader does not publish News-only changes', () => {
  assert.match(dashboardLoader, /const bool contentChanged = weatherChanged \|\| energyChanged;/);
  assert.doesNotMatch(dashboardLoader, /dashboardRevisions_\.news|newsCount_/);
});

test('dashboard loader retains a compact signature instead of the full JSON copy', () => {
  assert.match(dashboardLoader, /compatibility field name/);
  assert.match(dashboardLoader, /const std::string contentSignature =/);
  assert.match(
    dashboardLoader,
    /std::to_string\(sourceSize\) \+ ":" \+ std::to_string\(Fnv1a64\(text\)\)/,
  );
  assert.match(dashboardLoader, /dashboardUtf8_ = contentSignature;/);
  assert.doesNotMatch(dashboardLoader, /dashboardUtf8_ = std::move\(text\)/);
});

test('native playback state retains only a compact queue signature', () => {
  const updateStruct = rendererHeader.match(
    /struct NativePlaybackUpdate \{([\s\S]*?)\n  \};/,
  )?.[1] ?? '';
  assert.doesNotMatch(updateStruct, /std::wstring|fetchedAt/);
  assert.doesNotMatch(rendererHeader, /nativePlaybackRevision_/);
  assert.match(nativePlayback, /uint64_t PlaybackSnapshotSignature\(/);
  assert.match(nativePlayback, /update\.payloadSignature = projectionSignature;/);
  assert.doesNotMatch(nativePlayback, /PayloadSignature\(payload\)/);
  assert.doesNotMatch(nativePlayback, /update\.payload\s*=/);
  assert.doesNotMatch(nativePlayback, /update\.(?:source|error|fetchedAt)\s*=/);
});

test('playback snapshots are coalesced to queue changes or 30 minute checkpoints', () => {
  assert.match(nativePlayback, /kDashboardSnapshotCheckpointMs = 30 \* 60'000;/);
  assert.match(nativePlayback, /const bool snapshotChanged =\s*projectionSignature != lastSnapshotSignature;/s);
  assert.match(nativePlayback, /fetchedAt - lastSnapshotSavedAt >= kDashboardSnapshotCheckpointMs/);
  assert.match(nativePlayback, /\(snapshotChanged \|\| checkpointDue\) &&\s*SaveDashboardSnapshot/s);
  assert.doesNotMatch(
    nativePlayback,
    /ParseDashboardProjection[\s\S]*SaveDashboardSnapshot\(dataDir_, payload, \{\}, fetchedAt\);/,
  );
});

test('dashboard status and playback share one parsed root after validation', () => {
  assert.match(nativePlayback, /ParseDashboardStatus\(root, fetchedAt\)/);
  assert.doesNotMatch(nativePlayback, /ParseDashboardStatus\(payload, fetchedAt\)/);
  assert.match(nativePlayback, /ParseDashboardProjection\(\s*dataDir_, payload, fetchedAt, &statusProjection\)/s);
});

test('unchanged playback polls do not invalidate the full Music panel', () => {
  assert.match(nativePlayback, /bool musicChanged = false;/);
  assert.match(nativePlayback, /if \(musicChanged\) \{\s*InvalidatePanelSection\(nativeMainWindow_, PanelSection::Music\);/s);
  assert.doesNotMatch(nativePlayback, /\n    InvalidatePanelSection\(nativeMainWindow_, PanelSection::Music\);\n    std::unique_lock/);
});

test('playback update storage is reduced to one native source', () => {
  assert.match(rendererHeader, /NativePlaybackUpdate nativePlaybackUpdate_\{\};/);
  assert.doesNotMatch(rendererHeader, /nativePlaybackUpdates_|std::array<NativePlaybackUpdate/);
  assert.match(nativePlayback, /NativePlaybackUpdate& update = nativePlaybackUpdate_;/);
  assert.match(playbackResolve, /const NativePlaybackUpdate& update = nativePlaybackUpdate_;/);
  assert.doesNotMatch(nativePlayback, /nativePlaybackUpdates_/);
  assert.doesNotMatch(playbackResolve, /nativePlaybackUpdates_/);
});

test('artwork URL resolution avoids repeated cache directory and extension probes', () => {
  assert.match(artworkCache, /static thread_local MemoryIndex memoryIndex;/);
  assert.match(artworkCache, /const auto indexed = memoryIndex\.urls\.find\(artworkUrl\);/);
  assert.match(artworkCache, /if \(indexed != memoryIndex\.urls\.end\(\)\) return indexed->second;/);
  assert.match(artworkCache, /static thread_local fs::path preparedCacheDir;/);
  assert.match(artworkCache, /memoryIndex\.urls\.size\(\) >= 128/);
});

test('air history display remains five minute data while persistence is batched', () => {
  assert.match(airHistory, /kAirHistoryBucketMs = 5LL \* 60 \* 1000;/);
  assert.match(airHistory, /kAirHistoryPersistIntervalMs = 30LL \* 60 \* 1000;/);
  assert.match(
    airHistory,
    /now - lastAirHistorySavedAt_ >= kAirHistoryPersistIntervalMs[\s\S]*SaveAirHistory\(\)/,
  );
  assert.doesNotMatch(
    airHistory,
    /\+\+renderState_\.airHistoryRevision;\s*SaveAirHistory\(\);\s*MarkRenderStateDirty\(\);/s,
  );
});

test('logger keeps one stream open and avoids filesystem metadata checks per line', () => {
  assert.match(loggerHeader, /std::ofstream output_;/);
  assert.match(loggerHeader, /size_t currentBytes_ = 0;/);
  assert.match(loggerSource, /kLogFlushThresholdBytes = 64 \* 1024;/);
  assert.match(loggerSource, /output_\.write\(/);
  assert.doesNotMatch(loggerSource, /RotateIfNeeded/);
  const writeFunction = loggerSource.match(
    /void Logger::Write\([\s\S]*?\n\}/,
  )?.[0] ?? '';
  assert.doesNotMatch(writeFunction, /fs::exists|fs::file_size|std::ofstream output\(/);
});

test('static radar waits for updates instead of polling every five seconds', () => {
  assert.match(radarUi, /RadarAnimationIntervalFromSignature/);
  assert.match(radarUi, /if \(animationIntervalMs > 0\)[\s\S]*wait_for[\s\S]*else \{\s*radarComposeWake_\.wait/s);
  assert.match(radarUi, /frames\.Size\(\) > 1 \? frameIntervalMs : 0/);
  assert.match(radarUi, /L"\|animate:" << animationIntervalMs/);
});

test('animated radar avoids per-frame disk snapshot serialization', () => {
  assert.match(
    radarUi,
    /if \(animationIntervalMs == 0 && !signature\.empty\(\) &&\s*SaveBitmapAsBmp/s,
  );
  assert.match(
    radarUi,
    /if \(animationIntervalMs == 0 && !signature\.empty\(\) &&\s*file::MatchesText/s,
  );
});

test('radar updates invalidate only the radar window and reuse a source DC', () => {
  assert.match(radarUi, /InvalidateRadarWindow\(nativeRadarWindow_\);/);
  assert.doesNotMatch(radarUi, /InvalidateAllNativePanels\(\)/);
  assert.match(radarUi, /thread_local CachedRadarSourceDc cached/);
  assert.doesNotMatch(radarUi, /void BlendBitmap[\s\S]*DeleteDC\(sourceDc\)/);
});

test('native image caches use reduced LRU entry limits', () => {
  assert.match(bitmapCache, /kNativeImageBitmapCacheLimit = 16;/);
  assert.match(bitmapCache, /kRadarBitmapCacheLimit = 12;/);
  assert.doesNotMatch(bitmapCache, /kNativeImageBitmapCacheLimit = 24;/);
  assert.doesNotMatch(bitmapCache, /kRadarBitmapCacheLimit = 16;/);
});

test('native panel state no longer compares or invalidates News revisions', () => {
  assert.doesNotMatch(panelState, /newsIndexChanged|newsChanged|nativeNewsRenderRevision_/);
  assert.doesNotMatch(panelState, /PanelSection::News/);
});

test('News drawing function and paint branches are removed', () => {
  assert.doesNotMatch(dataSections, /DrawNewsSection|ニュース取得待ち/);
  assert.doesNotMatch(panelWindows, /PanelSection::News|sections\.news|nativeNewsRenderRevision_/);
  assert.doesNotMatch(layout, /sections\.news/);
});

test('playback projection runs only while the main panel is visible', () => {
  assert.match(
    panelState,
    /if \(nativeMainWindow_ && IsWindow\(nativeMainWindow_\) &&\s*IsWindowVisible\(nativeMainWindow_\)\) \{\s*const NativePlaybackTickState playbackState = NativePlaybackTickStateFor\(nowMs\);/s,
  );
});
