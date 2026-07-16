#include "web_renderer.h"

namespace hp {
bool InstallRuntimeAssets() noexcept;

namespace {
constexpr UINT_PTR kNativePanelTickTimer = 1;
constexpr UINT kNativePanelTickMs = 1'000;

HBRUSH DashboardBackgroundBrush() noexcept {
  static HBRUSH background = CreateSolidBrush(kNativeDashboardBackground);
  return background;
}

void PrepareParentWindow(HWND window) {
  SetClassLongPtrW(window, GCLP_HBRBACKGROUND,
                   reinterpret_cast<LONG_PTR>(DashboardBackgroundBrush()));
  const LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
  if ((style & WS_EX_NOREDIRECTIONBITMAP) != 0) {
    SetWindowLongPtrW(window, GWL_EXSTYLE, style & ~WS_EX_NOREDIRECTIONBITMAP);
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
  }
}
}  // namespace

Renderer::Renderer(HWND window, int width, int height)
    : window_(window), width_(width), height_(height) {
  wchar_t executable[MAX_PATH * 4]{};
  GetModuleFileNameW(nullptr, executable, _countof(executable));
  rootDir_ = fs::path(executable).parent_path();
  dataDir_ = rootDir_ / L"data";
  bounds_ = RECT{0, 0, width_, height_};
}

Renderer::~Renderer() {
  shuttingDown_ = true;
  StopNativePlaybackBridge();
  StopNativeMinuteFactsBridge();
  StopRadarCompose();
  DestroyNativeStaticWindows();
}

void Renderer::Initialize() {
  if (!InstallRuntimeAssets()) {
    throw std::runtime_error("runtime dashboard asset installation failed");
  }
  PrepareParentWindow(window_);
  EnsureNativeStaticWindows();
  StartNativePlaybackBridge();
  StartNativeMinuteFactsBridge();
  StartRadarCompose();
}

void Renderer::Resize(int width, int height) {
  const int nextWidth = std::max(1, width);
  const int nextHeight = std::max(1, height);
  if (width_ == nextWidth && height_ == nextHeight) return;
  width_ = nextWidth;
  height_ = nextHeight;
  ++nativeLayoutRevision_;
  bounds_.right = std::max(bounds_.left + 1L, bounds_.left + width_);
  bounds_.bottom = std::max(bounds_.top + 1L, bounds_.top + height_);
  ApplyNativeStaticBounds();
}

void Renderer::SetBounds(const RECT& bounds) {
  if (EqualRect(&bounds_, &bounds)) return;
  bounds_ = bounds;
  ++nativeLayoutRevision_;
  width_ = std::max(1L, bounds.right - bounds.left);
  height_ = std::max(1L, bounds.bottom - bounds.top);
  ApplyNativeStaticBounds();
}

void Renderer::SetVisible(bool visible) {
  const bool visibilityChanged = nativeDashboardVisible_ != visible;
  nativeDashboardVisible_ = visible;
  if (visibilityChanged) ApplyNativeStaticBounds();

  if (nativeMainWindow_ && IsWindow(nativeMainWindow_)) {
    if (visible) {
      if (!nativePanelTimerActive_) {
        nativePanelTimerActive_ =
            SetTimer(nativeMainWindow_, kNativePanelTickTimer, kNativePanelTickMs, nullptr) != 0;
      }
    } else if (nativePanelTimerActive_) {
      KillTimer(nativeMainWindow_, kNativePanelTickTimer);
      nativePanelTimerActive_ = false;
    }
  } else {
    nativePanelTimerActive_ = false;
  }
}

void Renderer::QueueAction(UiAction action) {
  {
    std::lock_guard lock(actionMutex_);
    pendingAction_ = action;
  }
  PostMessageW(window_, WM_LBUTTONUP, 0, MAKELPARAM(0, 0));
}

UiAction Renderer::TakePendingAction() {
  std::lock_guard lock(actionMutex_);
  const UiAction action = pendingAction_;
  pendingAction_ = UiAction::None;
  return action;
}

void Renderer::UpdateState(const RenderState& state) {
  UpdateNativeStaticPanels(state);
}

void Renderer::Render() {
  if (!window_ || !nativeDashboardVisible_) return;
  HDC dc = GetDC(window_);
  if (!dc) return;
  RECT bounds{};
  GetClientRect(window_, &bounds);
  FillRect(dc, &bounds, DashboardBackgroundBrush());
  ReleaseDC(window_, dc);
}

}  // namespace hp
