(() => {
  'use strict';

  if (window.__homepanelDashboardPerformance) return;

  const native = {
    setInterval: window.setInterval.bind(window),
    clearInterval: window.clearInterval.bind(window),
    setTimeout: window.setTimeout.bind(window),
    clearTimeout: window.clearTimeout.bind(window),
  };

  // HomePanel owns exactly three recurring dashboard jobs. Keep their normal
  // cadence while visible, but remove their native timers entirely while the
  // WebView is hidden instead of waking just to discover document.hidden.
  let nextManagedId = -1;
  const managedIntervals = new Map();

  const callbackSource = callback => {
    try {
      return typeof callback === 'function'
        ? Function.prototype.toString.call(callback)
        : String(callback);
    } catch (_) {
      return '';
    }
  };

  const managedKind = (source, delay) => {
    if (delay === 1000 && /\bupdateClock\b/.test(source)) return 'clock';
    if (delay === 5000 && /\btickSpProgress\b/.test(source)) return 'progress';
    if (delay === 5 * 60 * 1000 && /\brefreshRadar\b/.test(source)) return 'radar';
    return '';
  };

  const invoke = record => {
    if (typeof record.callback === 'function') {
      return record.callback.apply(window, record.args);
    }
    return (0, eval)(String(record.callback));
  };

  const nextDelay = record => {
    if (record.kind === 'clock') {
      return Math.max(20, 1008 - (Date.now() % 1000));
    }
    return record.delay;
  };

  const schedule = record => {
    if (!record.active || document.hidden || record.timerId) return;
    record.timerId = native.setTimeout(() => {
      record.timerId = 0;
      if (!record.active || document.hidden) return;
      try {
        invoke(record);
      } catch (error) {
        native.setTimeout(() => { throw error; }, 0);
      }
      schedule(record);
    }, nextDelay(record));
  };

  const clearManaged = id => {
    const record = managedIntervals.get(id);
    if (!record) return false;
    record.active = false;
    if (record.timerId) native.clearTimeout(record.timerId);
    managedIntervals.delete(id);
    return true;
  };

  window.setInterval = (callback, delay, ...args) => {
    const numericDelay = Math.max(0, Number(delay) || 0);
    const kind = managedKind(callbackSource(callback), numericDelay);
    if (!kind) return native.setInterval(callback, numericDelay, ...args);

    const id = nextManagedId--;
    const record = {
      kind,
      callback,
      args,
      delay: numericDelay,
      timerId: 0,
      active: true,
    };
    managedIntervals.set(id, record);
    schedule(record);
    return id;
  };

  window.clearInterval = id => {
    if (!clearManaged(id)) native.clearInterval(id);
  };
  window.clearTimeout = id => {
    if (!clearManaged(id)) native.clearTimeout(id);
  };

  document.addEventListener('visibilitychange', () => {
    for (const record of managedIntervals.values()) {
      if (record.timerId) native.clearTimeout(record.timerId);
      record.timerId = 0;
      schedule(record);
    }
  }, { passive: true });

  window.__homepanelDashboardPerformance = {
    get managedIntervalCount() { return managedIntervals.size; },
  };
})();

(() => {
  'use strict';

  if (window.__homepanelStationheadFetchNormalized) return;
  window.__homepanelStationheadFetchNormalized = true;

  const originalFetch = window.fetch.bind(window);
  const PLAYBACK_ORIGIN = 'https://skrzk.pages.dev';
  const PLAYBACK_PATH = '/api/playback';
  const nativePlaybackPayloads = new Map();
  const shared = window.__homepanelPlaybackShared || {};
  const asObject = shared.asObject || (value => value && typeof value === 'object' && !Array.isArray(value) ? value : {});
  const normalizePlaybackPayload = shared.normalizePlaybackPayload;

  function playbackSourceKey(input) {
    try {
      const raw = input instanceof Request ? input.url : String(input || '');
      const url = new URL(raw, location.href);
      if (url.origin !== PLAYBACK_ORIGIN || url.pathname !== PLAYBACK_PATH) return '';
      const channel = String(url.searchParams.get('channel') || 'buddies').toLowerCase();
      if (channel === 'buddy46') return 'b';
      if (channel === 'buddies') return 'a';
      return '';
    } catch (_) {
      return '';
    }
  }

  function isPlaybackRequest(input) {
    return Boolean(playbackSourceKey(input));
  }

  function responseFromPayload(payload) {
    return new Response(JSON.stringify(payload), {
      status: 200,
      statusText: 'OK',
      headers: { 'content-type': 'application/json; charset=utf-8' },
    });
  }

  function rememberNativePlayback(message) {
    const source = message?.source === 'b' ? 'b' : message?.source === 'a' ? 'a' : '';
    if (!source) return;
    const fetchedAt = Number(message.fetchedAt || Date.now()) || Date.now();
    if (message.payload && typeof message.payload === 'object') {
      nativePlaybackPayloads.set(source, {
        payload: normalizePlaybackPayload(message.payload, fetchedAt),
        fetchedAt,
        error: '',
      });
      window.dispatchEvent(new Event('online'));
      return;
    }
    nativePlaybackPayloads.set(source, {
      payload: null,
      fetchedAt,
      error: String(message.error || 'native playback fetch failed'),
    });
  }

  window.__homepanelNormalizeStationheadPlayback = normalizePlaybackPayload;
  window.__homepanelNativePlaybackPayloads = nativePlaybackPayloads;
  window.chrome?.webview?.addEventListener('message', event => {
    if (event.data?.type === 'native-playback') rememberNativePlayback(event.data);
  });

  window.fetch = async function homePanelFetch(input, init) {
    const source = playbackSourceKey(input);
    if (source) {
      const cached = nativePlaybackPayloads.get(source);
      if (cached?.payload) return responseFromPayload(cached.payload);
    }

    const response = await originalFetch(input, init);
    if (!isPlaybackRequest(input) || !response.ok) return response;

    try {
      const payload = await response.clone().json();
      const normalized = normalizePlaybackPayload(payload, Date.now());
      const headers = new Headers(response.headers);
      headers.delete('content-encoding');
      headers.delete('content-length');
      headers.set('content-type', 'application/json; charset=utf-8');
      return new Response(JSON.stringify(normalized), {
        status: response.status,
        statusText: response.statusText,
        headers,
      });
    } catch (_) {
      return response;
    }
  };
})();
