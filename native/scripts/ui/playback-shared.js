(() => {
  'use strict';

  if (window.__homepanelPlaybackShared) return;

  const asObject = value => value && typeof value === 'object' && !Array.isArray(value) ? value : {};

  function firstFinite(...values) {
    for (const value of values) {
      if (value === null || value === undefined || value === '') continue;
      const number = Number(value);
      if (Number.isFinite(number)) return number;
    }
    return 0;
  }

  function validQueueIndex(queue, ...values) {
    for (const value of values) {
      if (value === null || value === undefined || value === '') continue;
      const index = Number(value);
      if (Number.isInteger(index) && index >= 0 && index < queue.length) return index;
    }
    return queue.length ? 0 : -1;
  }

  function normalizeQueueItem(raw) {
    const item = asObject(raw);
    if (!Object.keys(item).length) return item;
    const normalized = { ...item };
    if (!normalized.imageUrl && item.thumbnail_url) normalized.imageUrl = item.thumbnail_url;
    if (!normalized.artworkUrl && item.thumbnail_url) normalized.artworkUrl = item.thumbnail_url;
    return normalized;
  }

  function normalizePlaybackPayload(raw, receivedAt = Date.now()) {
    const payload = asObject(raw);
    if (!Object.keys(payload).length) return raw;

    const queueStatus = asObject(payload.queue_status || payload.queueStatus);
    const queue = Array.isArray(payload.queue) ? payload.queue : [];
    const flaggedIndex = queue.findIndex(item => asObject(item).is_current === true);
    const currentIndex = validQueueIndex(
      queue,
      payload.currentIndex,
      payload.current_index,
      queueStatus.currentIndex,
      queueStatus.current_index,
      flaggedIndex >= 0 ? flaggedIndex : undefined,
    );
    const currentItem = currentIndex >= 0 ? asObject(queue[currentIndex]) : {};
    const progressMs = Math.max(0, firstFinite(
      payload.progressMs,
      payload.progress_ms,
      payload.positionMs,
      queueStatus.progressMs,
      queueStatus.progress_ms,
      currentItem.progressMs,
      currentItem.progress_ms,
    ));
    const serverAnchorAt = firstFinite(
      payload.anchorAt,
      payload.anchor_at,
      queueStatus.anchorAt,
      queueStatus.anchor_at,
    );
    const serverQueueEndAt = firstFinite(
      payload.queueEndAt,
      payload.queue_end_at,
      queueStatus.queueEndAt,
      queueStatus.queue_end_at,
    );
    const serverReferenceAt = firstFinite(
      payload.generated_at,
      payload.latest_observed_at,
      payload.sampledAt,
      payload.monitorSampledAt,
      payload.updatedAt,
      payload.queue_observed_at,
    );

    const normalized = { ...payload };
    normalized.currentIndex = Math.max(0, currentIndex);
    normalized.current_index = Math.max(0, currentIndex);
    normalized.progressMs = progressMs;
    normalized.progress_ms = progressMs;
    normalized.positionMs = progressMs;
    normalized.sampledAt = receivedAt;

    if (serverAnchorAt > 0) {
      const serverProgressMs = serverReferenceAt > 0
        ? Math.max(0, serverReferenceAt - serverAnchorAt)
        : progressMs;
      normalized.anchorAt = receivedAt - Math.max(progressMs, serverProgressMs);
      normalized.anchor_at = normalized.anchorAt;
    } else {
      normalized.anchorAt = 0;
      normalized.anchor_at = 0;
    }

    if (serverQueueEndAt > 0 && serverReferenceAt > 0) {
      normalized.queueEndAt = receivedAt + Math.max(0, serverQueueEndAt - serverReferenceAt);
    } else {
      normalized.queueEndAt = serverQueueEndAt;
    }
    normalized.queue_end_at = normalized.queueEndAt;

    if (typeof queueStatus.playing === 'boolean') {
      const playing = queueStatus.playing && queueStatus.is_paused !== true;
      normalized.playing = playing;
      normalized.is_paused = !playing;
    } else if (typeof queueStatus.is_paused === 'boolean') {
      normalized.is_paused = queueStatus.is_paused;
      if (queueStatus.is_paused) normalized.playing = false;
    }
    if (queue.length) normalized.queue = queue.map(normalizeQueueItem);

    return normalized;
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

  function formatTime(milliseconds) {
    const seconds = Math.max(0, Math.floor(Number(milliseconds || 0) / 1000));
    return `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}`;
  }

  window.__homepanelPlaybackShared = Object.freeze({
    asObject,
    firstFinite,
    formatTime,
    normalizedItem,
    normalizePlaybackPayload,
    normalizeQueueItem,
    queueFrom,
    validQueueIndex,
  });
})();
