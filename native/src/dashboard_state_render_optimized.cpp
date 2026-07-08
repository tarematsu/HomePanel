#include "web_renderer.h"

namespace hp {
void Renderer::Render(const RECT& dirty, const RenderState& state) {
  (void)dirty;
  (void)state;
  if (!window_) return;
  HDC dc = GetDC(window_);
  if (!dc) return;
  RECT bounds{};
  GetClientRect(window_, &bounds);
  FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
  ReleaseDC(window_, dc);
}

void Renderer::NotifyRadarUpdated() {
  if (!radarComposeStarted_.load(std::memory_order_acquire)) return;
  {
    std::lock_guard lock(radarComposeWakeMutex_);
    radarComposePending_ = true;
  }
  radarComposeWake_.notify_all();
}
}  // namespace hp
