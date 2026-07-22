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
const radarUi = readFileSync(
  new URL('../../native/src/renderer_radar_ui.cpp', import.meta.url),
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

test('native playback state retains only a compact response signature', () => {
  const updateStruct = rendererHeader.match(
    /struct NativePlaybackUpdate \{([\s\S]*?)\n  \};/,
  )?.[1] ?? '';
  assert.doesNotMatch(updateStruct, /std::wstring|fetchedAt/);
  assert.doesNotMatch(rendererHeader, /nativePlaybackRevision_/);
  assert.match(nativePlayback, /uint64_t PayloadSignature\(/);
  assert.match(nativePlayback, /update\.payloadSignature = payloadSignature;/);
  assert.doesNotMatch(nativePlayback, /update\.payload\s*=/);
  assert.doesNotMatch(nativePlayback, /update\.(?:source|error|fetchedAt)\s*=/);
});

test('playback update storage is reduced to one native source', () => {
  assert.match(rendererHeader, /NativePlaybackUpdate nativePlaybackUpdate_\{\};/);
  assert.doesNotMatch(rendererHeader, /nativePlaybackUpdates_|std::array<NativePlaybackUpdate/);
  assert.match(nativePlayback, /NativePlaybackUpdate& update = nativePlaybackUpdate_;/);
  assert.match(playbackResolve, /const NativePlaybackUpdate& update = nativePlaybackUpdate_;/);
  assert.doesNotMatch(nativePlayback, /nativePlaybackUpdates_/);
  assert.doesNotMatch(playbackResolve, /nativePlaybackUpdates_/);
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