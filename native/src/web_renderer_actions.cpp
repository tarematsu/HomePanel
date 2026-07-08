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
  const std::wstring json = BuildStateJson(state);
  PostState(json);
  statePublishedForPendingPaint_ = !json.empty();
}

void Renderer::PostState(const std::wstring& json) {
  if (json.empty() || !ready_ || !uiReady_ || !webview_) return;
  if (json == postedState_) return;
  webview_->PostWebMessageAsJson(json.c_str());
  postedState_ = json;
}

void Renderer::PostFullState() {
  const std::wstring json = BuildCachedStateJson(0, true);
  if (!json.empty()) postedState_.clear();
  PostState(json);
}

RECT Renderer::ClockRect() const { return ClientBounds(); }
RECT Renderer::SensorRect() const { return ClientBounds(); }
RECT Renderer::RadarRect() const { return ClientBounds(); }
RECT Renderer::StationheadRect() const { return ClientBounds(); }
}  // namespace hp
