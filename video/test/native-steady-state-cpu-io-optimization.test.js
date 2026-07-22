import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';

const appSource = readFileSync(
  new URL('../../native/src/app.cpp', import.meta.url),
  'utf8',
);
const appHeader = readFileSync(
  new URL('../../native/src/app.h', import.meta.url),
  'utf8',
);
const playbackResolve = readFileSync(
  new URL('../../native/src/dashboard_playback_resolve.cpp', import.meta.url),
  'utf8',
);
const rendererHeader = readFileSync(
  new URL('../../native/src/web_renderer.h', import.meta.url),
  'utf8',
);
const stationheadPlayerHeader = readFileSync(
  new URL('../../native/src/sh.h', import.meta.url),
  'utf8',
);
const stationheadHandles = readFileSync(
  new URL('../../native/src/app_stationhead_handles.cpp', import.meta.url),
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
const sensorSerial = readFileSync(
  new URL('../../native/src/sensors_serial.cpp', import.meta.url),
  'utf8',
);
const artworkCache = readFileSync(
  new URL('../../native/src/artwork_cache.h', import.meta.url),
  'utf8',
);

test('App timer follows real deadlines instead of a fixed five second dashboard cadence', () => {
  assert.doesNotMatch(appSource, /kSteadyDashboardTickMs/);
  assert.match(appSource, /stationhead_->NextWakeAt\(\)/);
  assert.match(appSource, /secondaryStationhead_->NextWakeAt\(\)/);
  assert.match(appSource, /renderer_->NativePlaybackNextWakeAt\(now\)/);
  assert.match(appSource, /constexpr uint32_t kMaxIdleTickMs = 30'000;/);
  assert.doesNotMatch(appSource, /selectedTab_ == WorkspaceTab::Main[\s\S]*kSteadyDashboardTickMs/);
});

test('native playback exposes an exact queue boundary deadline', () => {
  assert.match(rendererHeader, /int64_t NativePlaybackNextWakeAt\(int64_t nowMs\) const;/);
  assert.match(playbackResolve, /int64_t Renderer::NativePlaybackNextWakeAt\(int64_t nowMs\) const/);
  assert.match(playbackResolve, /projection\.queueEndAt <= nowMs \? nowMs : projection\.queueEndAt/);
  assert.match(playbackResolve, /duration \+ kPlaybackRenderTransitionHoldMs - position\.elapsedMs/);
});

test('steady Stationhead ticks read only the authorization flag', () => {
  assert.match(stationheadPlayerHeader, /bool SpotifyAuthorizationActive\(\) const/);
  assert.match(stationheadHandles, /player_->SpotifyAuthorizationActive\(\)/);
  assert.doesNotMatch(stationheadHandles, /player_->Status\(\)\.spotifyAuthorization/);
});

test('Stationhead play history persists on a fixed 30 minute cadence', () => {
  assert.match(stationheadHistory, /kPersistIntervalMs = 30LL \* 60 \* 1000;/);
  assert.match(stationheadHistory, /lastStationheadPlayHistorySavedAt_ = history\.empty\(\) \? 0 : now;/);
  assert.match(
    stationheadHistory,
    /now - lastStationheadPlayHistorySavedAt_ >= kPersistIntervalMs[\s\S]*SaveStationheadPlayHistory\(\)/,
  );
  assert.doesNotMatch(stationheadHistory, /currentValueChanged/);
});

test('batched histories flush dirty data on normal shutdown', () => {
  assert.match(appHeader, /struct HistoryFlushGuard/);
  assert.match(appHeader, /bool SaveAirHistory\(\) const;/);
  assert.match(appHeader, /bool SaveStationheadPlayHistory\(\) const;/);
  assert.match(appHeader, /bool airHistoryDirty_ = false;/);
  assert.match(appHeader, /bool stationheadPlayHistoryDirty_ = false;/);
  assert.match(
    airHistory,
    /HistoryFlushGuard::~HistoryFlushGuard\(\)[\s\S]*airHistoryDirty_[\s\S]*SaveAirHistory\(\)[\s\S]*stationheadPlayHistoryDirty_[\s\S]*SaveStationheadPlayHistory\(\)/,
  );
  assert.match(airHistory, /bool App::SaveAirHistory\(\) const/);
  assert.match(stationheadHistory, /bool App::SaveStationheadPlayHistory\(\) const/);
  assert.match(airHistory, /airHistoryDirty_ = true;/);
  assert.match(stationheadHistory, /stationheadPlayHistoryDirty_ = true;/);
});

test('missing serial sensors use bounded exponential retry backoff', () => {
  assert.match(sensorSerial, /kSerialRetryInitial = std::chrono::seconds\(10\)/);
  assert.match(sensorSerial, /kSerialRetryMaximum = std::chrono::seconds\(60\)/);
  assert.match(sensorSerial, /retryDelay = std::min\(retryDelay \* 2, kSerialRetryMaximum\)/);
  assert.match(sensorSerial, /if \(changed\) PostMessageW\(window_, WM_HP_SENSOR_UPDATED/);
  assert.doesNotMatch(sensorSerial, /wait_for\(lock, std::chrono::seconds\(10\)/);
});

test('artwork index capacity evicts one entry instead of clearing all entries', () => {
  assert.match(artworkCache, /memoryIndex\.urls\.erase\(memoryIndex\.urls\.begin\(\)\)/);
  assert.doesNotMatch(artworkCache, /size\(\) >= 128\) memoryIndex\.urls\.clear\(\)/);
});
