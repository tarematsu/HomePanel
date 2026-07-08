(() => {
  'use strict';

  const PLAYBACK_STALE_MS = 6 * 60 * 1000;
  const TRACK_TRANSITION_HOLD_MS = 1000;

  const PLAYBACK_SOURCES = Object.freeze({
    a: Object.freeze({ channel: 'buddies', prefix: 'stationhead-a' }),
    b: Object.freeze({ channel: 'buddy46', prefix: 'stationhead-b' }),
  });
  const PLAYBACK_SOURCE_KEYS = Object.freeze(Object.keys(PLAYBACK_SOURCES));

  let dashboard = {};
  let panelHeaderMetaHandled = false;
  const shared = window.__homepanelPlaybackShared;
  const utils = window.HomePanel?.utils || {};
  const panels = window.HomePanel?.panels || {};
  const asObject = shared.asObject;
  const normalizePlaybackPayload = shared.normalizePlaybackPayload;
  const formatTime = shared.formatTime;
  const normalizedItem = shared.normalizedItem;
  const queueFrom = shared.queueFrom;

  const playbackStates = {
    a: { value: {}, fetchedAt: 0, error: '', revision: 0 },
    b: { value: {}, fetchedAt: 0, error: '', revision: 0 },
  };
  const audioUiState = {
    a: { muted: false },
    b: { muted: false },
  };
  const snapshotCache = {
    key: '',
    a: null,
    b: null,
  };

  const $ = utils.$ || (selector => document.querySelector(selector));

  function removePanelHeaderMeta() {
    if (panelHeaderMetaHandled) return;
    $('.spotify-panel > header .subtle')?.remove();
    panelHeaderMetaHandled = true;
  }

  function postNativeAction(action, value) {
    const message = { type: 'action', action };
    if (Number.isFinite(Number(value))) message.value = Number(value);
    window.chrome?.webview?.postMessage(message);
  }

  function updateAudioControls(key) {
    const state = audioUiState[key];
    if (!state) return;
    const button = $(`#stationhead-${key}-audio`);
    if (!button) return;
    button.classList.toggle('is-muted', Boolean(state.muted));
    button.setAttribute('aria-pressed', state.muted ? 'true' : 'false');
    const label = button.querySelector('span') || button;
    const next = state.muted ? '音声OFF' : '音声ON';
    if (label.textContent !== next) label.textContent = next;
    button.title = state.muted ? 'クリックで音声ON' : 'クリックで音声OFF';
  }

  function syncAudioControlsFromRuntime(state) {
    const stationhead = state?.stationhead;
    if (!stationhead || typeof stationhead !== 'object') return;
    if (typeof stationhead.audioMuted === 'boolean') audioUiState.a.muted = stationhead.audioMuted;
    if (typeof stationhead.secondaryAudioMuted === 'boolean') audioUiState.b.muted = stationhead.secondaryAudioMuted;
    updateAudioControls('a');
    updateAudioControls('b');
  }

  function installAudioControlHandlers() {
    document.addEventListener('click', event => {
      const button = event.target.closest('.stationhead-audio-toggle[data-audio-window]');
      if (!button || button.disabled) return;
      const key = button.dataset.audioWindow;
      if (!audioUiState[key]) return;
      postNativeAction(key === 'a' ? 'stationhead-audio-a' : 'stationhead-audio-b');
    });
    updateAudioControls('a');
    updateAudioControls('b');
  }

  function setText(selector, value) {
    const node = $(selector);
    const next = String(value ?? '');
    if (node && node.textContent !== next) node.textContent = next;
  }

  function setBadge(prefix) {
    const badge = $(`#${prefix}-live-badge`);
    const text = $(`#${prefix}-live-text`);
    if (badge) {
      badge.hidden = true;
      if (badge.textContent) badge.textContent = '';
    }
    if (text) {
      text.hidden = true;
      if (text.textContent) text.textContent = '';
    }
  }

  function setArtwork(imageSelector, fallbackSelector, source) {
    const image = $(imageSelector);
    const fallback = $(fallbackSelector);
    if (!(image instanceof HTMLImageElement)) return;
    const url = String(source || '').trim();
    if (!url) {
      image.removeAttribute('src');
      image.hidden = true;
      if (fallback) fallback.hidden = false;
      return;
    }
    if (!image.dataset.homepanelArtworkEvents) {
      image.addEventListener('load', () => {
        image.hidden = false;
        if (fallback) fallback.hidden = true;
      });
      image.addEventListener('error', () => {
        image.hidden = true;
        if (fallback) fallback.hidden = false;
      });
      image.dataset.homepanelArtworkEvents = '1';
    }
    if (image.getAttribute('src') !== url) image.src = url;
    image.hidden = false;
    if (fallback) fallback.hidden = true;
  }

  function playerRow(prefix) {
    return document.getElementById(`${prefix}-track`)?.closest('.stationhead-player-row') || null;
  }

  function compactBody(prefix) {
    return playerRow(prefix)?.querySelector('.stationhead-compact-body') || null;
  }

  function ensureProgress(prefix) {
    const body = compactBody(prefix);
    if (!body) return null;
    let root = $(`#${prefix}-progress`);
    if (root) return root;
    root = document.createElement('div');
    root.id = `${prefix}-progress`;
    root.className = 'stationhead-progress';
    root.innerHTML = `<div class="stationhead-progress-track"><div id="${prefix}-progress-fill" class="stationhead-progress-fill"></div></div><div class="stationhead-progress-time"><span id="${prefix}-progress-current">0:00</span><span id="${prefix}-progress-duration">0:00</span></div>`;
    body.appendChild(root);
    return root;
  }

  function unwrapPlaybackPayload(raw) {
    const root = asObject(raw);
    if (typeof root.title === 'string' || typeof root.hasTrack === 'boolean') return root;
    if (root.resolved && typeof root.resolved === 'object') return asObject(root.resolved);
    const candidates = [root.playback, asObject(root.data).playback, root.data, root.stationhead, root.result, root];
    for (const candidate of candidates) {
      const value = asObject(candidate);
      if (Object.keys(value).length) return value;
    }
    return {};
  }

  function resolvedPlaybackSnapshot(value, now) {
    const durationMs = Math.max(0, Number(value.durationMs ?? 0) || 0);
    let progressMs = Math.max(0, Number(value.progressMs ?? value.positionMs ?? 0) || 0);
    const playing = value.playing === true;
    const anchorAt = Number(value.anchorAt ?? 0) || 0;
    const sampledAt = Number(value.sampledAt ?? 0) || 0;
    if (playing) {
      if (anchorAt > 0) progressMs = Math.max(0, now - anchorAt);
      else if (sampledAt > 0) progressMs += Math.max(0, now - sampledAt);
    }
    if (durationMs > 0) progressMs = Math.min(Math.max(0, progressMs), durationMs);
    return {
      title: String(value.title || '').trim(),
      artist: String(value.artist || '').trim(),
      artwork: String(value.artwork || '').trim(),
      durationMs,
      progressMs,
      playing,
      hasTrack: value.hasTrack === true || Boolean(value.title),
    };
  }

  function projectedElapsedMs({ playing, progressMs, anchorAt, sampledAt }, now) {
    if (!playing) return progressMs;
    if (anchorAt > 0) return Math.max(0, now - anchorAt);
    if (sampledAt > 0) return progressMs + Math.max(0, now - sampledAt);
    return progressMs;
  }

  function resolveQueueSnapshot(value, queue, playback, now) {
    let index = Number(value.currentIndex ?? value.current_index ?? value.queue_status?.current_index ?? -1);
    if (!Number.isInteger(index) || index < 0 || index >= queue.length) {
      index = queue.findIndex(item => item?.is_current === true);
    }
    if (!Number.isInteger(index) || index < 0 || index >= queue.length) index = 0;

    let elapsed = projectedElapsedMs(playback, now);
    while (index < queue.length) {
      const queueItem = normalizedItem(queue[index]);
      if (!playback.playing ||
          queueItem.durationMs <= 0 ||
          elapsed < queueItem.durationMs + TRACK_TRANSITION_HOLD_MS) {
        break;
      }
      elapsed -= queueItem.durationMs;
      index += 1;
    }

    if (index >= queue.length) {
      return { itemSource: {}, durationMs: 0, progressMs: 0 };
    }

    const itemSource = queue[index];
    const queueItem = normalizedItem(itemSource);
    return {
      itemSource,
      durationMs: queueItem.durationMs,
      progressMs: Math.min(queueItem.durationMs, Math.max(0, elapsed)),
    };
  }

  function rawPlaybackSnapshot(value, now) {
    const playing = value.playing === true || (value.is_broadcasting === true && value.is_paused !== true);
    const sampledAt = Number(value.sampledAt ?? value.monitorSampledAt ?? value.updatedAt ?? 0) || 0;
    const anchorAt = Number(value.anchorAt ?? 0) || 0;
    const playback = {
      playing,
      progressMs: Math.max(0, Number(value.progressMs ?? value.positionMs ?? 0) || 0),
      anchorAt,
      sampledAt,
    };
    let itemSource = value.item || value.currentItem || value.currentTrack || value.track || {};
    let durationMs = Math.max(0, Number(value.durationMs ?? value.trackDurationMs ?? asObject(itemSource).durationMs ?? 0) || 0);
    let progressMs = playback.progressMs;
    const queue = queueFrom(value);

    if (queue.length) {
      const queueSnapshot = resolveQueueSnapshot(value, queue, playback, now);
      itemSource = queueSnapshot.itemSource;
      durationMs = queueSnapshot.durationMs;
      progressMs = queueSnapshot.progressMs;
    } else {
      const expectedEndAt = Number(value.expectedEndAt ?? 0) || 0;
      if (durationMs > 0 && expectedEndAt > 0) {
        progressMs = durationMs - Math.max(0, expectedEndAt + TRACK_TRANSITION_HOLD_MS - now);
      } else if (playing && sampledAt > 0) {
        progressMs = projectedElapsedMs(playback, now);
      }
    }

    const item = normalizedItem(itemSource);
    if (!durationMs) durationMs = item.durationMs;
    if (durationMs > 0) progressMs = Math.min(Math.max(0, progressMs), durationMs);
    return {
      title: item.name,
      artist: item.artist,
      artwork: item.artwork,
      durationMs,
      progressMs,
      playing,
      hasTrack: Boolean(item.name),
    };
  }

  function playbackSnapshot(rawValue, now = Date.now()) {
    const value = unwrapPlaybackPayload(rawValue);
    if (Object.prototype.hasOwnProperty.call(value, 'hasTrack') &&
        Object.prototype.hasOwnProperty.call(value, 'durationMs')) {
      return resolvedPlaybackSnapshot(value, now);
    }
    return rawPlaybackSnapshot(value, now);
  }

  function renderProgress(prefix, snapshot) {
    const root = ensureProgress(prefix);
    if (!root) return;
    root.hidden = !(snapshot.hasTrack && snapshot.durationMs > 0);
    if (root.hidden) return;
    const ratio = Math.max(0, Math.min(1, snapshot.progressMs / snapshot.durationMs));
    const fill = $(`#${prefix}-progress-fill`);
    if (fill) {
      fill.style.display = 'block';
      fill.style.width = '100%';
      fill.style.margin = '0';
      fill.style.transformOrigin = 'left center';
      fill.style.transform = `scaleX(${ratio.toFixed(4)})`;
      fill.style.transition = 'transform .25s linear';
    }
    setText(`#${prefix}-progress-current`, formatTime(snapshot.progressMs));
    setText(`#${prefix}-progress-duration`, formatTime(snapshot.durationMs));
  }

  function sourceIsStale(source, now = Date.now()) {
    return source.fetchedAt <= 0 || now - source.fetchedAt > PLAYBACK_STALE_MS;
  }

  function sourceStatus(source, snapshot, now = Date.now()) {
    const stale = sourceIsStale(source, now);
    if (source.error && stale) return { kind: 'stale', detail: `${source.error}` };
    if (source.error) return { kind: 'offline', detail: `${source.error}` };
    if (stale) return { kind: 'stale', detail: 'native更新待ち' };
    if (snapshot.playing && snapshot.hasTrack) return { kind: 'live', detail: '' };
    if (snapshot.hasTrack) return { kind: 'paused', detail: '停止中' };
    return { kind: 'offline', detail: '情報待ち' };
  }

  function renderStationheadPlayer({ key, source, snapshot, status }) {
    setText(`#${source.prefix}-track`, snapshot.title || `${source.channel}曲情報待ち`);
    setText(`#${source.prefix}-artist`, snapshot.artist || '--');
    setArtwork(`#${source.prefix}-artwork`, `#${source.prefix}-artwork-fallback`, snapshot.artwork);
    renderProgress(source.prefix, snapshot);
    setBadge(source.prefix, status.kind, status.detail);
    updateAudioControls(key);
  }

  function playerRenderModel(key, snapshots) {
    const snapshot = snapshots[key];
    return {
      key,
      source: PLAYBACK_SOURCES[key],
      snapshot,
      status: sourceStatus(playbackStates[key], snapshot),
    };
  }

  function renderPlayers() {
    const snapshots = currentSnapshots();
    for (const key of PLAYBACK_SOURCE_KEYS) {
      renderStationheadPlayer(playerRenderModel(key, snapshots));
    }
  }

  function currentSnapshots() {
    const cacheKey = JSON.stringify([
      ...PLAYBACK_SOURCE_KEYS.flatMap(key => [
        playbackStates[key].revision,
        playbackStates[key].fetchedAt,
      ]),
      Math.floor(Date.now() / 1000),
    ]);
    if (snapshotCache.key === cacheKey &&
        PLAYBACK_SOURCE_KEYS.every(key => Boolean(snapshotCache[key]))) return snapshotCache;
    snapshotCache.key = cacheKey;
    for (const key of PLAYBACK_SOURCE_KEYS) {
      snapshotCache[key] = playbackSnapshot(playbackStates[key].value);
    }
    return snapshotCache;
  }

  function hasActiveProgress() {
    const snapshots = currentSnapshots();
    return PLAYBACK_SOURCE_KEYS.some(key => {
      const snapshot = snapshots[key];
      return snapshot.hasTrack && snapshot.durationMs > 0;
    });
  }

  function renderAll() {
    removePanelHeaderMeta();
    panels.switchbot?.renderEnergyPlugs?.(dashboard.switchbot || {});
    renderPlayers();
  }

  function applyNativePlayback(message) {
    const key = String(message?.source || '').toLowerCase();
    if (!playbackStates[key]) return;
    playbackStates[key].fetchedAt = Number(message.fetchedAt) || Date.now();
    playbackStates[key].error = String(message.error || '');
    playbackStates[key].revision += 1;
    playbackStates[key].value = message.payload && typeof message.payload === 'object'
      ? normalizePlaybackPayload(unwrapPlaybackPayload(message.payload), playbackStates[key].fetchedAt)
      : message.resolved && typeof message.resolved === 'object'
        ? message.resolved
        : {};
    renderAll();
  }

  function applyState(state) {
    const incoming = state || {};
    if (incoming.dashboard) dashboard = incoming.dashboard;
    syncAudioControlsFromRuntime(incoming);
    renderAll();
  }

  document.addEventListener('visibilitychange', () => {
    if (!document.hidden) renderAll();
  });
  window.addEventListener('homepanel-second', () => {
    if (document.hidden || !hasActiveProgress()) return;
    renderPlayers();
  });
  window.chrome?.webview?.addEventListener('message', event => {
    const message = event.data || {};
    if (message.type === 'native-playback') applyNativePlayback(message);
    else applyState(message);
  });

  removePanelHeaderMeta();
  installAudioControlHandlers();
  renderAll();
})();
