#include "web_renderer.h"

namespace hp {
void Renderer::QueueAction(UiAction action, float seekFraction) {
  {
    std::lock_guard lock(actionMutex_);
    pendingAction_ = action;
    pendingSeekFraction_ = std::clamp(seekFraction, 0.0f, 1.0f);
  }
  PostMessageW(window_, WM_LBUTTONUP, 0, MAKELPARAM(0, 0));
}

UiAction Renderer::HitTest(POINT, float* seekFraction) {
  UiAction action = UiAction::None;
  float pendingSeek = 0.0f;
  {
    std::lock_guard lock(actionMutex_);
    action = pendingAction_;
    pendingSeek = pendingSeekFraction_;
    pendingAction_ = UiAction::None;
    pendingSeekFraction_ = 0.0f;
  }
  if (seekFraction) *seekFraction = pendingSeek;
  return action;
}

void Renderer::UpdateState(const RenderState& state) {
  UpdateNativeStaticPanels(state);
}

RECT Renderer::ClockRect() const { return ComputeNativeDashboardLayout(ClientBounds()).clock; }
RECT Renderer::SensorRect() const { return ComputeNativeDashboardLayout(ClientBounds()).air; }
RECT Renderer::RadarRect() const { return ComputeNativeDashboardLayout(ClientBounds()).radar; }
RECT Renderer::StationheadRect() const {
  return ComputeNativeDashboardLayout(ClientBounds()).stationhead;
}
}  // namespace hp
