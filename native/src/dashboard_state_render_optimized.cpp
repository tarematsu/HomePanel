#include "web_renderer.h"

namespace hp {
void Renderer::Render(const RECT& dirty, const RenderState& state) {
  (void)state;
  if (!ready_) {
    statePublishedForPendingPaint_ = false;
    DrawStartupFallback(dirty);
    return;
  }
  if (statePublishedForPendingPaint_) {
    statePublishedForPendingPaint_ = false;
    FlushNativePlaybackMessages();
    return;
  }
  FlushNativePlaybackMessages();
}

void Renderer::NotifyRadarUpdated() {
  radarUpdatePending_ = true;
  if (!ready_ || !uiReady_ || !webview_) return;
  webview_->PostWebMessageAsJson(L"{\"type\":\"radar-updated\"}");
  radarUpdatePending_ = false;
}
}  // namespace hp
