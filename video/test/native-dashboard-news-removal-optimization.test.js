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
const stationheadHistory = readFileSync(
  new URL('../../native/src/app_stationhead_history.cpp', import.meta.url),
  'utf8',
);
const stationheadHandles = readFileSync(
  new URL('../../native/src/app_stationhead_handles.cpp', import.meta.url),
  'utf8',
);
const stationheadPlayerHeader = readFileSync(
  new URL('../../native/src/sh.h', import.meta.url),
  'utf8',
);
const sensorSerial = readFileSync(
  new URL('../../native/src/sensors_serial.cpp', import.meta.url),
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

test('playback snapshots persist only compact playback and minute facts', () => {
  assert.match(nativePlayback, /kCompactSnapshotVersion = 2;/);
  assert.match(nativePlayback, /bool SaveDashboardSnapshot\([\s\S]*const NativePlaybackProjection& playback[\s\S]*const NativeMinuteFactsProjection& facts/s);
  assert.match(nativePlayback, /L",\\"playback\\":\{"/);
  assert.match(nativePlayback, /L"\]\},\\"facts\\":\{"/);
  assert.match(nativePlayback, /bool LoadCompactSnapshot\(/);
  assert.match(nativePlayback, /Read snapshots created by pre-v2 builds once/);
  const saveFunction = nativePlayback.match(
    /bool SaveDashboardSnapshot\([\s\S]*?\n\}/,
  )?.[0] ?? '';
  assert.doesNotMatch(saveFunction, /payload/);
});

test('playback snapshot writes use queue persistence changes or 30 minute checkpoints', () => {
  assert.match(nativePlayback, /kDashboardSnapshotCheckpointMs = 30 \* 60'000;/);
  assert.match(nativePlayback, /uint64_t PlaybackPersistenceSignature\(/);
  assert.match(nativePlayback, /const bool snapshotChanged =\s*persistenceSignature != lastPersistenceSignature;/s);
  assert.match(nativePlayback, /fetchedAt - lastSnapshotSavedAt >= kDashboardSnapshotCheckpointMs/);
  assert.match(nativePlayback, /\(snapshotChanged \|\| checkpointDue\) &&\s*SaveDashboardSnapshot/s);
});

test('dashboard response JSON is parsed once for playback and status', () => {
  assert.match(nativePlayback, /bool ParseDashboardPayload\(/);
  assert.match(nativePlayback, /facts = ParseDashboardStatus\(root, fetchedAt\)/);
  const fetchFunction = nativePlayback.match(
    /std::wstring FetchDashboardJson\([\s\S]*?\n\}/,
  )?.[0] ?? '';
  assert.doesNotMatch(fetchFunction, /JsonObject::Parse|JsonValue::Parse/);
  assert.match(nativePlayback, /ParseDashboardPayload\(\s*dataDir_, payload, fetchedAt, &projection, &statusProjection, &error\)/s);
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

test('artwork URL resolution avoids repeated probes, backs off failures, and evicts one entry', () => {
  assert.match(artworkCache, /static thread_local MemoryIndex memoryIndex;/);
  assert.match(artworkCache, /const auto indexed = memoryIndex\.urls\.find\(artworkUrl\);/);
  assert.match(artworkCache, /kArtworkFailureRetryMs = 30 \* 60'000/);
  assert.match(artworkCache, /now < indexed->second\.retryAfter/);
  assert.match(artworkCache, /remember\(artworkUrl, now \+ kArtworkFailureRetryMs\)/);
  assert.match(artworkCache, /static thread_local fs::path preparedCacheDir;/);
  assert.match(artworkCache, /memoryIndex\.urls\.size\(\) >= 128/);
  assert.match(artworkCache, /memoryIndex\.urls\.erase\(memoryIndex\.urls\.begin\(\)\)/);
  assert.doesNotMatch(artworkCache, /size\(\) >= 128\) memoryIndex\.urls\.clear\(\)/);
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

test('Stationhead play history persists on a fixed 30 minute cadence', () => {
  assert.match(stationheadHistory, /kPersistIntervalMs = 30LL \* 60 \* 1000;/);
  assert.match(stationheadHistory, /lastStationheadPlayHistorySavedAt_ = history\.empty\(\) \? 0 : now;/);
  assert.match(
    stationheadHistory,
    /bucket - lastStationheadPlayHistorySavedAt_ >= kPersistIntervalMs[\s\S]*SaveStationheadPlayHistory\(\)/,
  );
  assert.doesNotMatch(stationheadHistory, /currentValueChanged/);
  assert.doesNotMatch(stationheadHistory, /currentValueChanged \|\|/);
});

test('steady Stationhead ticks read only the authorization flag', () => {
  assert.match(stationheadPlayerHeader, /bool SpotifyAuthorizationActive\(\) const/);
  assert.match(stationheadHandles, /player_->SpotifyAuthorizationActive\(\)/);
  assert.doesNotMatch(stationheadHandles, /player_->Status\(\)\.spotifyAuthorization/);
});

test('missing serial sensors use bounded exponential retry backoff', () => {
  assert.match(sensorSerial, /kSerialRetryInitial = std::chrono::seconds\(10\)/);
  assert.match(sensorSerial, /kSerialRetryMaximum = std::chrono::seconds\(60\)/);
  assert.match(sensorSerial, /const auto waitForRetry/);
  assert.match(sensorSerial, /retryDelay = std::min\(retryDelay \* 2, kSerialRetryMaximum\)/);
  assert.match(sensorSerial, /if \(changed\) PostMessageW\(window_, WM_HP_SENSOR_UPDATED/);
  assert.doesNotMatch(sensorSerial, /wait_for\(lock, std::chrono::seconds\(10\)/);
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

test('clock second ticks redraw only the time while footer refreshes by minute', () => {
  assert.match(panelState, /const bool clockMinuteChanged = previousClockSecondKey < 0 \|\|/);
  assert.match(panelState, /clockDayChanged \|\| clockMinuteChanged[\s\S]*PanelSection::Clock[\s\S]*PanelSection::ClockTime/s);
  assert.match(
    layout,
    /RECT timeRect\{content\.left, content\.top \+ SpanY\(content, 170\),\s*content\.right, content\.top \+ SpanY\(content, 705\)\}/s,
  );
  assert.doesNotMatch(layout, /Include the footer in the clock tick invalidation/);
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
