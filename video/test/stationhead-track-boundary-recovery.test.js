import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';

const header = readFileSync(
  new URL('../../native/src/sh_track_boundary_script.h', import.meta.url),
  'utf8',
);
const rawScript = header.match(/LR"JS\(([\s\S]*?)\)JS";/)?.[1];
assert.ok(rawScript, 'Stationhead track-boundary script must remain extractable');
const script = rawScript.replaceAll('{{PREFIX}}', 'stationhead');

function page({ markerAgeMs = null } = {}) {
  const now = 1_000_000;
  const timers = [];
  const storage = new Map();
  if (markerAgeMs !== null) {
    storage.set(
      '__homepanelStationheadRecentPlayback:stationhead',
      String(now - markerAgeMs),
    );
  }

  class HTMLMediaElement {}
  HTMLMediaElement.HAVE_METADATA = 1;
  HTMLMediaElement.HAVE_CURRENT_DATA = 2;

  const media = new HTMLMediaElement();
  Object.assign(media, {
    paused: true,
    ended: false,
    readyState: HTMLMediaElement.HAVE_CURRENT_DATA,
    isConnected: true,
    currentSrc: 'https://cdn.example/live.mp3',
    tagName: 'AUDIO',
    playCalls: 0,
    getAttribute: () => '',
    querySelector: () => null,
    async play() {
      this.playCalls += 1;
      this.paused = false;
    },
  });

  const document = {
    querySelectorAll: selector => selector === 'audio,video' ? [media] : [],
    addEventListener() {},
  };
  const window = {
    location: { hostname: 'www.stationhead.com' },
    setTimeout(callback) {
      timers.push(callback);
      return timers.length;
    },
    clearTimeout() {},
    addEventListener() {},
    chrome: { webview: { postMessage() {} } },
  };
  const context = vm.createContext({
    window,
    document,
    location: window.location,
    sessionStorage: {
      getItem: key => storage.get(key) ?? null,
      setItem: (key, value) => storage.set(key, value),
    },
    HTMLMediaElement,
    Promise,
    Number,
    Array,
    Boolean,
    Date: class extends Date {
      static now() { return now; }
    },
  });
  vm.runInContext(script, context, {
    filename: 'stationhead-track-boundary-script.js',
  });
  return { media, timers };
}

test('track-boundary recovery does not autoplay a first-time or login page', () => {
  const state = page();
  assert.equal(state.timers.length, 0);
  assert.equal(state.media.playCalls, 0);
});

test('track-boundary recovery resumes recent established playback after navigation', async () => {
  const state = page({ markerAgeMs: 60_000 });
  assert.equal(state.timers.length, 1);

  state.timers.shift()();
  await Promise.resolve();
  await Promise.resolve();

  assert.equal(state.media.playCalls, 1);
  assert.equal(state.media.paused, false);
});

test('track-boundary recovery ignores stale playback markers', () => {
  const state = page({ markerAgeMs: 31 * 60_000 });
  assert.equal(state.timers.length, 0);
  assert.equal(state.media.playCalls, 0);
});
