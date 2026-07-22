#include "web_renderer.h"

namespace hp {

NativeMinuteFactsProjection Renderer::NativeMinuteFactsSnapshot() const {
  std::lock_guard lock(nativeMinuteFactsMutex_);
  return nativeMinuteFacts_;
}

}  // namespace hp
