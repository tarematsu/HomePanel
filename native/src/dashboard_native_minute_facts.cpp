#include "web_renderer.h"

namespace hp {

void Renderer::StartNativeMinuteFactsBridge() {
  nativeMinuteFactsStarted_ = true;
  nativeMinuteFactsStopping_ = false;
}

void Renderer::StopNativeMinuteFactsBridge() noexcept {
  nativeMinuteFactsStopping_ = true;
  nativeMinuteFactsStarted_ = false;
}

void Renderer::NativeMinuteFactsLoop() {
  // Stationhead status is projected once from the dashboard JSON fetched by
  // NativePlaybackLoop. A second polling thread would duplicate the request.
}

NativeMinuteFactsProjection Renderer::NativeMinuteFactsSnapshot() const {
  std::lock_guard lock(nativeMinuteFactsMutex_);
  return nativeMinuteFacts_;
}

}  // namespace hp
