(() => {
  'use strict';

  const PLAYBACK_STALE_MS = 6 * 60 * 1000;
  const SPOTIFY_STALE_GRACE_MS = 90 * 1000;
  const SPOTIFY_POLL_MS = 5 * 60 * 1000;

  const PLAYBACK_SOURCES = Object.freeze({
    a: Object.freeze({ channel: 'buddies', prefix: 'stationhead-a', rowSelector: '.stationhead-player-row[aria-label="StationheadウインドウA"]' }),
    b: Object.freeze({ channel: 'buddy46', prefix: 'stationhead-b', rowSelector: '.stationhead-player-row[aria-label="StationheadウインドウB"]' }),
  });

  let runtime = {};
  let dashboard = {};
  let spotifyState = {};
  let progressTimer = 0;
  let panelHeaderMetaHandled = false;

  const playbackStates = {
    a: { value: {}, fetchedAt: 0, error: '', revision: 0 },
    b: { value: {}, fetchedAt: 0, error: '', revision: 0 },
  };
  const audioUiState = {
    a: { muted: false },
    b: { muted: false },
  };

  const $ = selector => document.querySelector(selector);
  const finite = value => value !== null && value !== undefined && value !== '' && Number.isFinite(Number(value));
  const escapeHtml = value => String(value ?? '').replace(/[&<>'"]/g, character => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', "'": '&#39;', '"': '&quot;',
  }[character]));

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
    if (button) {
      button.classList.toggle('is-muted', Boolean(state.muted));
      button.setAttribute('aria-pressed', state.muted ? 'true' : 'false');
      const label = button.querySelector('span') || button;
      const next = state.muted ? '音声OFF' : '音声ON';
      if (label.textContent !== next) label.textContent = next;
      button.title = state.muted ? 'クリックで音声ON' : 'クリックで音声OFF';
    }
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
      audioUiState[key].muted = !audioUiState[key].muted;
      updateAudioControls(key);
    });
    updateAudioControls('a');
    updateAudioControls('b');
  }

  function renderPlugs(value) {
    const container = $('#energy-plugs');
    if (!container) return;
    const devices = Array.isArray(value?.devices) ? value.devices : [];
    const plugs = devices.filter(device => /Plug Mini/i.test(String(device.deviceType || '')));
    const signature = JSON.stringify([
      value?.serviceAvailable, value?.__status, value?.degradedReason, value?.__error,
      plugs.map(device => [device.deviceId, device.deviceName, device.watts, device.power, device.error]),
    ]);
    if (container.dataset.signature === signature) return;
    container.dataset.signature = signature;
    const available = value?.serviceAvailable !== false && !['stale', 'error', 'waiting'].includes(value?.__status);
    if (!plugs.length) {
      container.innerHTML = `<div class="plug-empty">${available ? 'Plug Mini情報なし' : 'Plug Mini情報を取得できません'}</div>`;
      return;
    }
    container.innerHTML = plugs.map(device => {
      const online = available && !device.error;
      const raw = online ? String(device.power || '').trim().toUpperCase() : '';
      const state = raw === 'ON' ? 'ON' : raw === 'OFF' ? 'OFF' : '--';
      const watts = online && finite(device.watts) ? `${Number(device.watts).toFixed(1)} W` : '-- W';
      const name = device.deviceName || device.deviceId || 'Plug Mini';
      const reason = device.error || value?.degradedReason || value?.__error || '';
      const title = reason ? ` title="${escapeHtml(reason)}"` : '';
      return `<div class="energy-plug${online ? '' : ' is-unavailable'}"${title}><span class="energy-plug-name" title="${escapeHtml(name)}">${escapeHtml(name)}</span><span class="energy-plug-watts">${escapeHtml(watts)}</span><b class="energy-plug-state ${state === 'ON' ? 'is-on' : state === 'OFF' ? 'is-off' : 'is-unknown'}">${state}</b></div>`;
    }).join('');
  }

  function setText(selector, value) {
    const node = $(selector);
    const next = String(value ?? '');
    if (node && node.textContent !== next) node.textContent = next;
  }

  function setBadge(prefix, kind, detail) {
    const badge = $(`#${prefix}-live-badge`);
    const text = $(`#${prefix}-live-text`);
    const labels = { live: 'LIVE', paused: 'PAUSED', stale: 'STALE', offline: 'OFFLINE' };
    if (badge) {
      const className = `sp-live-badge ${kind}`;
      if (badge.className !== className) badge.className = className;
      const label = labels[kind] || labels.offline;
      if (badge.textContent !== label) badge.textContent = label;
    }
    if (text && text.textContent !== detail) text.textContent = detail;
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

  function formatTime(milliseconds) {
    const seconds = Math.max(0, Math.floor(Number(milliseconds || 0) / 1000));
    return `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}`;
  }

  function ensureProgress(prefix, bodySelector) {
    const body = $(bodySelector);
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

  function asObject(value) {
    return value && typeof value === 'object' && !Array.isArray(value) ? value : {};
  }

  function unwrapPlaybackPayload(raw) {
    const root = asObject(raw);
    const candidates = [root.playback, asObject(root.data).playback, root.data, root.stationhead, root.result, root];
    for (const candidate of candidates) {
      const value = asObject(candidate);
      if (Object.keys(value).length) return value;
    }
    return {};
  }

  function queueFrom(value) {
    const currentStation = asObject(value.currentStation || value.current_station);
    const channel = asObject(value.channel);
    const channelStation = asObject(channel.currentStation || channel.current_station);
    return [value.queue, currentStation.queue, channelStation.queue, channel.queue].find(Array.isArray) || [];
  }

  function normalizedItem(value) {
    const source = asObject(value?.track || value?.song || value);
    const artistValue = source.artist ?? source.artists;
    const artist = Array.isArray(artistValue)
      ? artistValue.map(item => typeof item === 'string' ? item : item?.name).filter(Boolean).join(', ')
      : typeof artistValue === 'object'
        ? artistValue?.name
        : artistValue;
    const album = asObject(source.album);
    const images = Array.isArray(album.images) ? album.images : Array.isArray(source.images) ? source.images : [];
    return {
      name: String(source.name || source.title || source.trackTitle || '').trim(),
      artist: String(artist || source.trackArtist || '').trim(),
      artwork: source.artwork || source.artworkUrl || source.albumArtUrl || source.image || source.imageUrl || source.thumbnail_url || images[0]?.url || '',
      durationMs: Math.max(0, Number(source.durationMs ?? source.duration_ms ?? source.lengthMs ?? 0) || 0),
    };
  }

  function playbackSnapshot(rawValue, now = Date.now()) {
    const value = unwrapPlaybackPayload(rawValue);
    const playing = value.playing === true || (value.is_broadcasting === true && value.is_paused !== true);
    const sampledAt = Number(value.sampledAt ?? value.monitorSampledAt ?? value.updatedAt ?? 0) || 0;
    const anchorAt = Number(value.anchorAt ?? 0) || 0;
    const queueEndAt = Number(value.queueEndAt ?? 0) || 0;
    const queue = queueFrom(value);

    let itemSource = value.item || value.currentItem || value.currentTrack || value.track || {};
    let durationMs = Math.max(0, Number(value.durationMs ?? value.trackDurationMs ?? asObject(itemSource).durationMs ?? 0) || 0);
    let progressMs = Math.max(0, Number(value.progressMs ?? value.positionMs ?? 0) || 0);

    if (queue.length) {
      let index = Number(value.currentIndex ?? value.current_index ?? value.queue_status?.current_index ?? -1);
      if (!Number.isInteger(index) || index < 0 || index >= queue.length) index = queue.findIndex(item => item?.is_current === true);
      if (!Number.isInteger(index) || index < 0 || index >= queue.length) index = 0;
      let elapsed = progressMs;
      if (playing) {
        if (anchorAt > 0) elapsed = Math.max(0, now - anchorAt);
        else if (sampledAt > 0) elapsed += Math.max(0, now - sampledAt);
      }
      while (index < queue.length) {
        const queueItem = normalizedItem(queue[index]);
        if (!playing || queueItem.durationMs <= 0 || elapsed <= queueItem.durationMs) break;
        elapsed -= queueItem.durationMs;
        index += 1;
      }
      if (index < queue.length) {
        itemSource = queue[index];
        const queueItem = normalizedItem(itemSource);
        durationMs = queueItem.durationMs;
        progressMs = Math.max(0, elapsed);
      } else {
        itemSource = {};
        durationMs = 0;
        progressMs = 0;
      }
    } else {
      const expectedEndAt = Number(value.expectedEndAt ?? 0) || 0;
      if (durationMs > 0 && expectedEndAt > 0) progressMs = durationMs - Math.max(0, expectedEndAt - now);
      else if (playing && sampledAt > 0) progressMs += Math.max(0, now - sampledAt);
    }

    const item = normalizedItem(itemSource);
    if (!durationMs) durationMs = item.durationMs;
    if (durationMs > 0) progressMs = Math.min(Math.max(0, progressMs), durationMs);
    return { title: item.name, artist: item.artist, artwork: item.artwork, durationMs, progressMs, playing, hasTrack: Boolean(item.name) };
  }

  function spotifySnapshot(rawValue, now = Date.now()) {
    const value = asObject(rawValue);
    const item = normalizedItem(value.item || value.track || {});
    const durationMs = Math.max(0, Number(value.durationMs ?? item.durationMs ?? 0) || 0);
    let progressMs = Math.max(0, Number(value.progressMs ?? value.positionMs ?? 0) || 0);
    const playing = value.playing === true;
    const sampledAt = Number(value.sampledAt ?? 0) || 0;
    const expectedEndAt = Number(value.expectedEndAt ?? 0) || 0;
    if (durationMs > 0 && expectedEndAt > 0) progressMs = durationMs - Math.max(0, expectedEndAt - now);
    else if (playing && sampledAt > 0) progressMs += Math.max(0, now - sampledAt);
    if (durationMs > 0) progressMs = Math.min(Math.max(0, progressMs), durationMs);
    return { title: item.name, artist: item.artist, artwork: item.artwork, durationMs, progressMs, playing, hasTrack: Boolean(item.name) };
  }

  function mergeSnapshots(primary, fallback) {
    if (!fallback?.hasTrack) return primary;
    if (primary?.hasTrack && primary?.artwork) return primary;
    return {
      title: primary?.title || fallback.title,
      artist: primary?.artist || fallback.artist,
      artwork: primary?.artwork || fallback.artwork,
      durationMs: primary?.durationMs || fallback.durationMs,
      progressMs: primary?.progressMs || fallback.progressMs,
      playing: primary?.playing === true || fallback.playing === true,
      hasTrack: Boolean(primary?.hasTrack || fallback.hasTrack),
    };
  }

  function renderProgress(prefix, bodySelector, snapshot) {
    const root = ensureProgress(prefix, bodySelector);
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

  function sourceStatus(source, snapshot, now = Date.now()) {
    const stale = source.fetchedAt <= 0 || now - source.fetchedAt > PLAYBACK_STALE_MS;
    if (source.error && stale) return { kind: 'stale', detail: `${source.error}` };
    if (source.error) return { kind: 'offline', detail: `${source.error}` };
    if (stale) return { kind: 'stale', detail: 'native更新待ち' };
    if (snapshot.playing && snapshot.hasTrack) return { kind: 'live', detail: 'native playback' };
    if (snapshot.hasTrack) return { kind: 'paused', detail: '停止中' };
    return { kind: 'offline', detail: '情報待ち' };
  }

  function authorizationRequired(value) {
    return value?.reauthorizationRequired === true || /authorization required|authorization is not configured/i.test(String(value?.error || ''));
  }

  function spotifyAvailability(value, now = Date.now()) {
    const sampledAt = Number(value?.sampledAt ?? 0);
    const stale = sampledAt <= 0 || now - sampledAt > SPOTIFY_POLL_MS + SPOTIFY_STALE_GRACE_MS;
    const authRequired = authorizationRequired(value);
    const apiError = Boolean(value?.error);
    const connected = value?.connected !== false;
    const live = !stale && !authRequired && !apiError && connected && value?.playing === true;
    if (live) return { kind: 'live', detail: 'Spotify API / buddy46', offline: false };
    if (authRequired) return { kind: 'offline', detail: 'Spotify再連携が必要です', offline: true };
    if (apiError || !connected) return { kind: 'offline', detail: 'Spotify API取得エラー', offline: true };
    if (stale) return { kind: 'offline', detail: 'Spotify API更新待ち', offline: true };
    return { kind: 'offline', detail: 'Spotify再生なし', offline: true };
  }

  function setPanelOffline(rowSelector, offline) {
    const row = $(rowSelector);
    if (!row) return;
    row.classList.toggle('is-api-offline', Boolean(offline));
    row.style.filter = offline ? 'grayscale(1) saturate(.2)' : '';
    row.style.opacity = offline ? '.42' : '';
    row.style.transition = 'filter .2s ease, opacity .2s ease';
  }

  function renderStationheadPlayer({ source, snapshot, status, grayOut = false }) {
    const bodySelector = `${source.rowSelector} .stationhead-compact-body`;
    setText(`#${source.prefix}-track`, snapshot.title || `${source.channel}曲情報待ち`);
    setText(`#${source.prefix}-artist`, snapshot.artist || '--');
    setArtwork(`#${source.prefix}-artwork`, `#${source.prefix}-artwork-fallback`, snapshot.artwork);
    renderProgress(source.prefix, bodySelector, snapshot);
    setBadge(source.prefix, status.kind, status.detail);
    setPanelOffline(source.rowSelector, grayOut);
    updateAudioControls(source === PLAYBACK_SOURCES.b ? 'b' : 'a');
  }

  function renderAll() {
    removePanelHeaderMeta();
    renderPlugs(dashboard.switchbot || {});

    const sourceA = PLAYBACK_SOURCES.a;
    const snapshotA = playbackSnapshot(playbackStates.a.value);
    renderStationheadPlayer({ source: sourceA, snapshot: snapshotA, status: sourceStatus(playbackStates.a, snapshotA) });

    const sourceB = PLAYBACK_SOURCES.b;
    const snapshotB = mergeSnapshots(playbackSnapshot(playbackStates.b.value), spotifySnapshot(spotifyState));
    const spotifyStatus = spotifyAvailability(spotifyState);
    renderStationheadPlayer({
      source: sourceB,
      snapshot: snapshotB,
      status: sourceStatus(playbackStates.b, snapshotB).kind === 'live' ? sourceStatus(playbackStates.b, snapshotB) : spotifyStatus,
      grayOut: spotifyStatus.offline && !snapshotB.hasTrack,
    });
  }

  function applyNativePlayback(message) {
    const key = String(message?.source || '').toLowerCase();
    if (!playbackStates[key]) return;
    playbackStates[key].fetchedAt = Number(message.fetchedAt) || Date.now();
    playbackStates[key].error = String(message.error || '');
    playbackStates[key].revision += 1;
    playbackStates[key].value = message.payload && typeof message.payload === 'object'
      ? unwrapPlaybackPayload(message.payload)
      : {};
    renderAll();
  }

  function scheduleProgress() {
    if (progressTimer) clearTimeout(progressTimer);
    progressTimer = setTimeout(() => {
      progressTimer = 0;
      if (!document.hidden) renderAll();
      scheduleProgress();
    }, 1000);
  }

  function applyState(state) {
    const incoming = state || {};
    runtime = { ...runtime, ...incoming };
    if (incoming.dashboard) dashboard = incoming.dashboard;
    if (Object.prototype.hasOwnProperty.call(incoming, 'spotify')) {
      spotifyState = incoming.spotify && typeof incoming.spotify === 'object' ? incoming.spotify : {};
    }
    syncAudioControlsFromRuntime(incoming);
    renderAll();
  }

  document.addEventListener('visibilitychange', () => {
    if (!document.hidden) renderAll();
  });
  window.chrome?.webview?.addEventListener('message', event => {
    const message = event.data || {};
    if (message.type === 'native-playback') applyNativePlayback(message);
    else applyState(message);
  });

  removePanelHeaderMeta();
  installAudioControlHandlers();
  renderAll();
  scheduleProgress();
})();
