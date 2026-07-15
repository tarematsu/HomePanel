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

struct ProjectedTrackPosition {
  size_t index = 0;
  int64_t elapsedMs = 0;
};

ProjectedTrackPosition ResolveProjectedTrackPosition(const NativePlaybackProjection& projection,
                                                     int64_t nowMs) {
  ProjectedTrackPosition position;
  position.index = projection.currentIndex > 0 &&
          projection.currentIndex < static_cast<int>(projection.queue.size())
      ? static_cast<size_t>(projection.currentIndex)
      : 0;
  position.elapsedMs = ProjectedElapsedMs(projection, nowMs);
  while (position.index < projection.queue.size()) {
    const int64_t duration = projection.queue[position.index].durationMs;
    if (!projection.playing || duration <= 0 ||
        position.elapsedMs < duration + kPlaybackRenderTransitionHoldMs) {
      break;
    }
    position.elapsedMs -= duration;
    ++position.index;
  }
  return position;
}

bool PlaybackHasRenderableTrack(const NativePlaybackProjection& projection, int64_t nowMs) {
  if (!projection.available || projection.queue.empty()) return false;
  if (projection.playing && projection.queueEndAt > 0 && nowMs >= projection.queueEndAt) {
    return false;
  }
  const ProjectedTrackPosition position = ResolveProjectedTrackPosition(projection, nowMs);
  return position.index < projection.queue.size() &&
         !projection.queue[position.index].title.empty();
}

bool PlaybackEndedWithoutNextTrack(const NativePlaybackProjection& projection, int64_t nowMs) {
  if (!projection.available || !projection.playing || projection.queue.empty()) return false;
  size_t index = projection.currentIndex >= 0 &&
          projection.currentIndex < static_cast<int>(projection.queue.size())
      ? static_cast<size_t>(projection.currentIndex)
      : 0;
  if (!TrackHasIdentity(projection.queue[index])) return false;
  if (projection.queueEndAt > 0 && nowMs >= projection.queueEndAt) return true;

  int64_t elapsed = ProjectedElapsedMs(projection, nowMs);
  while (index < projection.queue.size()) {
    const int64_t duration = projection.queue[index].durationMs;
    if (duration <= 0 || elapsed < duration + kPlaybackRenderTransitionHoldMs) return false;
    elapsed -= duration;
    ++index;
    if (index >= projection.queue.size()) return true;
    if (!TrackHasIdentity(projection.queue[index])) return true;
  }
  return true;
}
}  // namespace

NativePlaybackRender Renderer::ResolveNativePlaybackLocked(size_t source, int64_t nowMs) const {
  NativePlaybackRender render;
  if (source >= nativePlaybackUpdates_.size()) return render;
  const NativePlaybackProjection& projection = nativePlaybackUpdates_[source].projection;
  if (!projection.available || projection.queue.empty()) return render;
  render.available = true;
  render.playing = projection.playing;
  if (projection.playing && projection.queueEndAt > 0 && nowMs >= projection.queueEndAt) {
    return render;
  }

  const ProjectedTrackPosition position = ResolveProjectedTrackPosition(projection, nowMs);
  if (position.index >= projection.queue.size()) return render;

  render.track = projection.queue[position.index];
  render.hasTrack = !render.track.title.empty();
  render.progressMs = std::max<int64_t>(0, position.elapsedMs);
  if (render.track.durationMs > 0) {
    render.progressMs = std::min(render.progressMs, render.track.durationMs);
  }
  return render;
}

NativePlaybackRender Renderer::ResolveNativePlayback(size_t source, int64_t nowMs) const {
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
  status.hasTrack = PlaybackHasRenderableTrack(projection, nowMs);
  status.endedWithoutNextTrack = PlaybackEndedWithoutNextTrack(projection, nowMs);
  status.contentRevision = update.contentRevision;
  return status;
}

bool Renderer::NativePlaybackActive(int64_t nowMs) const {
  std::lock_guard lock(nativePlaybackMutex_);
  for (const NativePlaybackUpdate& update : nativePlaybackUpdates_) {
    const NativePlaybackProjection& projection = update.projection;
    if (!projection.available || !projection.playing || projection.queue.empty()) continue;
    if (projection.queueEndAt > 0 && nowMs >= projection.queueEndAt) continue;
    const ProjectedTrackPosition position = ResolveProjectedTrackPosition(projection, nowMs);
    if (position.index >= projection.queue.size()) continue;
    const NativePlaybackTrack& track = projection.queue[position.index];
    if (!track.title.empty() && track.durationMs > 0) return true;
  }
  return false;
}
}  // namespace hp
