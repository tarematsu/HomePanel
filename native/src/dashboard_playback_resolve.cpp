#include "web_renderer.h"

namespace hp {
namespace {
constexpr int64_t kPlaybackRenderTransitionHoldMs = 500;

int64_t ProjectedElapsedMs(const NativePlaybackProjection& projection, int64_t nowMs) {
  if (!projection.playing) return projection.progressMs;
  if (projection.anchorAt > 0) return std::max<int64_t>(0, nowMs - projection.anchorAt);
  if (projection.sampledAt > 0) {
    return projection.progressMs + std::max<int64_t>(0, nowMs - projection.sampledAt);
  }
  return projection.progressMs;
}

bool TrackHasIdentity(const NativePlaybackTrack& track) {
  return !track.title.empty() || !track.artist.empty() || !track.artwork.empty();
}

bool IsPlaybackFallbackUrl(const std::wstring& url, const std::wstring& fallbackUrl) {
  return !url.empty() && !fallbackUrl.empty() &&
         _wcsicmp(url.c_str(), fallbackUrl.c_str()) == 0;
}

bool UsesSecondaryPlaybackFeed(const StationheadStatus& state) {
  return IsPlaybackFallbackUrl(state.url, state.fallbackUrl) &&
         IsPlaybackFallbackUrl(state.secondaryUrl, state.fallbackUrl);
}

struct ProjectedTrackPosition {
  size_t index = 0;
  int64_t elapsedMs = 0;
};

struct ProjectionCursor {
  const NativePlaybackProjection* owner = nullptr;
  const NativePlaybackTrack* queueData = nullptr;
  size_t queueSize = 0;
  int currentIndex = 0;
  int64_t progressMs = 0;
  int64_t anchorAt = 0;
  int64_t sampledAt = 0;
  int64_t fetchedAt = 0;
  int64_t queueEndAt = 0;
  bool playing = false;
  size_t index = 0;
  int64_t consumedMs = 0;
  int64_t resolvedAt = 0;
};

ProjectionCursor& CursorFor(const NativePlaybackProjection& projection) {
  thread_local std::array<ProjectionCursor, 4> cursors{};
  thread_local size_t replacement = 0;
  for (ProjectionCursor& cursor : cursors) {
    if (cursor.owner == &projection) return cursor;
  }
  for (ProjectionCursor& cursor : cursors) {
    if (!cursor.owner) {
      cursor.owner = &projection;
      return cursor;
    }
  }
  ProjectionCursor& cursor = cursors[replacement++ % cursors.size()];
  cursor = {};
  cursor.owner = &projection;
  return cursor;
}

bool CursorMatches(const ProjectionCursor& cursor,
                   const NativePlaybackProjection& projection) {
  return cursor.queueData == projection.queue.data() &&
         cursor.queueSize == projection.queue.size() &&
         cursor.currentIndex == projection.currentIndex &&
         cursor.progressMs == projection.progressMs &&
         cursor.anchorAt == projection.anchorAt &&
         cursor.sampledAt == projection.sampledAt &&
         cursor.fetchedAt == projection.fetchedAt &&
         cursor.queueEndAt == projection.queueEndAt &&
         cursor.playing == projection.playing;
}

void ResetCursor(ProjectionCursor& cursor,
                 const NativePlaybackProjection& projection,
                 size_t startIndex) {
  cursor.queueData = projection.queue.data();
  cursor.queueSize = projection.queue.size();
  cursor.currentIndex = projection.currentIndex;
  cursor.progressMs = projection.progressMs;
  cursor.anchorAt = projection.anchorAt;
  cursor.sampledAt = projection.sampledAt;
  cursor.fetchedAt = projection.fetchedAt;
  cursor.queueEndAt = projection.queueEndAt;
  cursor.playing = projection.playing;
  cursor.index = startIndex;
  cursor.consumedMs = 0;
  cursor.resolvedAt = 0;
}

ProjectedTrackPosition ResolveProjectedTrackPosition(const NativePlaybackProjection& projection,
                                                     int64_t nowMs) {
  const bool hasCurrentTrack = projection.currentIndex >= 0 &&
      projection.currentIndex < static_cast<int>(projection.queue.size());
  const size_t startIndex = hasCurrentTrack
      ? static_cast<size_t>(projection.currentIndex)
      : projection.queue.size();
  const int64_t elapsed = ProjectedElapsedMs(projection, nowMs);
  if (!projection.playing || projection.queue.empty()) {
    return {startIndex, elapsed};
  }

  ProjectionCursor& cursor = CursorFor(projection);
  if (!CursorMatches(cursor, projection) || cursor.index < startIndex ||
      cursor.index > projection.queue.size() || nowMs < cursor.resolvedAt ||
      elapsed < cursor.consumedMs) {
    ResetCursor(cursor, projection, startIndex);
  }

  int64_t remaining = elapsed - cursor.consumedMs;
  while (cursor.index < projection.queue.size()) {
    const int64_t duration = projection.queue[cursor.index].durationMs;
    if (duration <= 0 || remaining < duration + kPlaybackRenderTransitionHoldMs) break;
    remaining -= duration;
    cursor.consumedMs += duration;
    ++cursor.index;
  }
  cursor.resolvedAt = nowMs;
  return {cursor.index, remaining};
}

bool PlaybackEndedWithoutNextTrack(const NativePlaybackProjection& projection, int64_t nowMs) {
  if (!projection.available || projection.setupRequired) return false;
  if (projection.ended) return true;
  if (!projection.playing || projection.queue.empty() || projection.currentIndex < 0 ||
      projection.currentIndex >= static_cast<int>(projection.queue.size())) {
    return false;
  }
  const size_t startIndex = static_cast<size_t>(projection.currentIndex);
  if (!TrackHasIdentity(projection.queue[startIndex])) return false;
  if (projection.queueEndAt > 0 && nowMs >= projection.queueEndAt) return true;

  const ProjectedTrackPosition position = ResolveProjectedTrackPosition(projection, nowMs);
  if (position.index == startIndex) return false;
  if (position.index >= projection.queue.size()) return true;
  return !TrackHasIdentity(projection.queue[position.index]);
}
}  // namespace

NativePlaybackRender Renderer::ResolveNativePlaybackLocked(size_t source, int64_t nowMs) const {
  NativePlaybackRender render;
  if (source >= nativePlaybackUpdates_.size()) return render;
  const NativePlaybackProjection& projection = nativePlaybackUpdates_[source].projection;
  render.available = projection.available;
  render.playing = projection.playing;
  render.stale = projection.stale;
  render.ended = projection.ended;
  render.setupRequired = projection.setupRequired;
  if (!projection.available || projection.queue.empty() || projection.currentIndex < 0 ||
      projection.currentIndex >= static_cast<int>(projection.queue.size())) {
    return render;
  }
  if (projection.playing && projection.queueEndAt > 0 && nowMs >= projection.queueEndAt) {
    return render;
  }

  const ProjectedTrackPosition position = ResolveProjectedTrackPosition(projection, nowMs);
  if (position.index >= projection.queue.size()) return render;

  render.track = projection.queue[position.index];
  render.hasTrack = TrackHasIdentity(render.track);
  render.progressMs = std::max<int64_t>(0, position.elapsedMs);
  if (render.track.durationMs > 0) {
    render.progressMs = std::min(render.progressMs, render.track.durationMs);
  }
  return render;
}

NativePlaybackRender Renderer::ResolveNativePlayback(size_t source, int64_t nowMs) const {
  const size_t selectedSource = UsesSecondaryPlaybackFeed(nativeStationhead_) ? 1 : 0;
  if (source != selectedSource) return {};
  std::lock_guard lock(nativePlaybackMutex_);
  return ResolveNativePlaybackLocked(source, nowMs);
}

NativePlaybackFeedStatus Renderer::NativePlaybackFeedStatusFor(size_t source,
                                                               int64_t nowMs) const {
  std::lock_guard lock(nativePlaybackMutex_);
  NativePlaybackFeedStatus status;
  if (source >= nativePlaybackUpdates_.size()) return status;
  const NativePlaybackUpdate& update = nativePlaybackUpdates_[source];
  const NativePlaybackProjection& projection = update.projection;
  status.available = projection.available;
  status.playing = projection.playing;
  if (projection.currentIndex >= 0 &&
      projection.currentIndex < static_cast<int>(projection.queue.size())) {
    status.hasTrack = TrackHasIdentity(
        projection.queue[static_cast<size_t>(projection.currentIndex)]);
  }
  status.endedWithoutNextTrack = PlaybackEndedWithoutNextTrack(projection, nowMs);
  status.contentRevision = update.contentRevision;
  return status;
}

Renderer::NativePlaybackTickState Renderer::NativePlaybackTickStateFor(int64_t nowMs) const {
  NativePlaybackTickState state;
  state.source = UsesSecondaryPlaybackFeed(nativeStationhead_) ? 1 : 0;
  std::lock_guard lock(nativePlaybackMutex_);
  if (state.source >= nativePlaybackUpdates_.size()) return state;

  const NativePlaybackUpdate& update = nativePlaybackUpdates_[state.source];
  const NativePlaybackProjection& projection = update.projection;
  state.contentRevision = update.contentRevision;
  if (!projection.available || projection.queue.empty() || projection.currentIndex < 0 ||
      projection.currentIndex >= static_cast<int>(projection.queue.size())) {
    return state;
  }
  if (projection.playing && projection.queueEndAt > 0 && nowMs >= projection.queueEndAt) {
    return state;
  }

  const ProjectedTrackPosition position = ResolveProjectedTrackPosition(projection, nowMs);
  state.trackIndex = position.index;
  if (position.index >= projection.queue.size()) return state;
  const NativePlaybackTrack& track = projection.queue[position.index];
  state.active = projection.playing && !track.title.empty() && track.durationMs > 0;
  return state;
}
}  // namespace hp
