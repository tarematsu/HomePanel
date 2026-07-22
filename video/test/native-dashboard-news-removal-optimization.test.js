import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';

const dashboardParser = readFileSync(
  new URL('../../native/src/dashboard_data.cpp', import.meta.url),
  'utf8',
);
const dashboardLoader = readFileSync(
  new URL('../../native/src/renderer_dashboard.cpp', import.meta.url),
  'utf8',
);
const panelState = readFileSync(
  new URL('../../native/src/renderer_panel_state.cpp', import.meta.url),
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

test('dashboard loader does not arm News rotation or publish News-only changes', () => {
  assert.match(dashboardLoader, /const bool contentChanged = weatherChanged \|\| energyChanged;/);
  assert.match(dashboardLoader, /newsCount_ = 0;/);
  assert.doesNotMatch(dashboardLoader, /dashboardRevisions_\.news/);
});

test('dashboard loader retains a compact signature instead of the full JSON copy', () => {
  assert.match(dashboardLoader, /const std::string contentSignature =/);
  assert.match(dashboardLoader, /std::to_string\(sourceSize\).*Fnv1a64\(text\)/s);
  assert.match(dashboardLoader, /dashboardUtf8_ = contentSignature;/);
  assert.doesNotMatch(dashboardLoader, /dashboardUtf8_ = std::move\(text\)/);
});

test('native panel state no longer compares or invalidates News revisions', () => {
  assert.doesNotMatch(panelState, /newsIndexChanged|newsChanged|nativeNewsRenderRevision_/);
  assert.doesNotMatch(panelState, /PanelSection::News/);
});

test('playback projection work is guarded by visible main-panel state', () => {
  assert.match(
    panelState,
    /if \(nativeMainWindow_ && IsWindow\(nativeMainWindow_\) &&\s*IsWindowVisible\(nativeMainWindow_\)\) \{\s*const NativePlaybackTickState playbackState = NativePlaybackTickStateFor\(nowMs\);/s,
  );
});

test('legacy News rectangle remains outside the visible main client', () => {
  assert.match(
    layout,
    /sections\.news = RECT\{client\.right, client\.bottom, client\.right \+ 1, client\.bottom \+ 1\};/,
  );
});
