// Part of sh_player.cpp's translation unit (see the #include at the end of that
// file). Window/host management for the primary Stationhead player: host-window
// creation and the foreground/behind-dashboard/1x1 controller placement.
#include "sh.h"

namespace hp {

bool StationheadPlayer::EnsureHostWindow() {
  if (!window_ || !IsWindow(window_)) return false;
  if (hostWindow_ && IsWindow(hostWindow_)) return true;
  static constexpr wchar_t kHostClassName[] = L"HomePanelStationheadHost";
  static std::once_flag classOnce;
  std::call_once(classOnce, [] {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kHostClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
  });
  hostWindow_ = CreateWindowExW(0, kHostClassName, L"StationheadHost",
      WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 1, 1, window_, nullptr,
      GetModuleHandleW(nullptr), nullptr);
  return hostWindow_ && IsWindow(hostWindow_);
}

bool StationheadPlayer::EnsureAuthHostWindow() {
  if (!window_ || !IsWindow(window_)) return false;
  if (authHostWindow_ && IsWindow(authHostWindow_)) return true;
  static constexpr wchar_t kAuthHostClassName[] = L"HomePanelSpotifyAuthHost";
  static std::once_flag classOnce;
  std::call_once(classOnce, [] {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kAuthHostClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
  });
  authHostWindow_ = CreateWindowExW(0, kAuthHostClassName, L"SpotifyAuthHost",
      WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 1, 1, window_, nullptr,
      GetModuleHandleW(nullptr), nullptr);
  controllerLayoutValid_ = false;
  return authHostWindow_ && IsWindow(authHostWindow_);
}

void StationheadPlayer::LayoutHostWindow(bool background) {
  if (!EnsureHostWindow()) return;
  if (background) KeepPlaybackBehindDashboard();
  else {
    LayoutControllers();
    viewVisible_ = true;
    std::lock_guard lock(mutex_);
    status_.visible = true;
  }
}

void StationheadPlayer::KeepPlaybackBehindDashboard() {
  if (!controller_ || !EnsureHostWindow()) {
    viewVisible_ = false;
    std::lock_guard lock(mutex_);
    status_.visible = false;
    return;
  }
  // Collapse the backgrounded player to a 1x1 surface: the GPU process stops
  // compositing a full-size hidden WebView, and a 1x1 child window can no
  // longer paint over the dashboard. Audio playback is independent of the
  // surface size; interactive states resize back via LayoutControllers().
  controller_->put_ZoomFactor(1.0);
  controller_->put_Bounds(RECT{0, 0, 1, 1});
  controller_->put_IsVisible(TRUE);
  ApplyMute();
  SetWindowRgn(hostWindow_, nullptr, FALSE);
  ShowWindow(hostWindow_, SW_SHOWNOACTIVATE);
  SetWindowPos(hostWindow_, HWND_BOTTOM, bounds_.left, bounds_.top, 1, 1,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
  backgroundHostPlaced_ = true;
  controllerLayoutValid_ = false;
  if (authController_) authController_->put_IsVisible(FALSE);
  if (authHostWindow_ && IsWindow(authHostWindow_)) ShowWindow(authHostWindow_, SW_HIDE);
  viewVisible_ = false;
  std::lock_guard lock(mutex_);
  status_.visible = false;
}

void StationheadPlayer::SetVisible(bool visible) {
  if (!visible) {
    selectedTab_ = StationheadTabKind::None;
    if (controller_) KeepPlaybackBehindDashboard();
    else {
      viewVisible_ = false;
      std::lock_guard lock(mutex_);
      status_.visible = false;
    }
    if (window_ && IsWindow(window_)) SetFocus(window_);
    return;
  }
  if (selectedTab_ == StationheadTabKind::None && !NeedsInteractiveWindow()) {
    selectedTab_ = StationheadTabKind::Stationhead;
  }
  if (!controller_) {
    viewVisible_ = selectedTab_ != StationheadTabKind::None || NeedsInteractiveWindow();
    std::lock_guard lock(mutex_);
    status_.visible = viewVisible_;
    return;
  }
  if (selectedTab_ == StationheadTabKind::None && !NeedsInteractiveWindow()) {
    KeepPlaybackBehindDashboard();
    return;
  }
  const bool wasVisible = viewVisible_;
  viewVisible_ = true;
  LayoutControllers();
  ApplyMute();
  // Primary Stationhead A never comes to the front. Even the Spotify auth
  // surface stays behind the dashboard and is driven by automation/state
  // transitions rather than direct foreground interaction.
  if (selectedTab_ == StationheadTabKind::Auth && authHostWindow_ && IsWindow(authHostWindow_)) {
    ShowWindow(authHostWindow_, SW_SHOWNOACTIVATE);
    SetWindowPos(authHostWindow_, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
  if (!wasVisible && controller_) controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

void StationheadPlayer::LayoutControllers() {
  const int width = std::max(1L, bounds_.right - bounds_.left);
  const int height = std::max(1L, bounds_.bottom - bounds_.top);
  // A background (not visible) player collapses to 1x1 so the GPU stops
  // compositing a full-size hidden WebView and its child window can't paint
  // over the dashboard. Audio is size-independent; the auth/foreground states
  // keep the full size below.
  const int hostWidth = viewVisible_ ? width : 1;
  const int hostHeight = viewVisible_ ? height : 1;
  const RECT controllerBounds{0, 0, hostWidth, hostHeight};
  const bool showAuth = selectedTab_ == StationheadTabKind::Auth && authController_;
  if (controller_) {
    controller_->put_Bounds(controllerBounds);
    controller_->put_IsVisible(showAuth ? FALSE : TRUE);
  }
  if (hostWindow_ && IsWindow(hostWindow_)) {
    if (showAuth) ShowWindow(hostWindow_, SW_HIDE);
    else {
      ShowWindow(hostWindow_, SW_SHOWNOACTIVATE);
      // Stationhead content always stays behind the dashboard WebView (a
      // sibling child of the same parent) so it never covers the dashboard.
      // Auto-play relies on layout geometry + CDP input, which work while the
      // window is occluded/behind, so it never needs to come to the front.
      SetWindowPos(hostWindow_, HWND_BOTTOM,
                   bounds_.left, bounds_.top, hostWidth, hostHeight, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
  }
  if (authController_) {
    authController_->put_Bounds(controllerBounds);
    authController_->put_IsVisible(showAuth ? TRUE : FALSE);
  }
  if (authHostWindow_ && IsWindow(authHostWindow_)) {
    if (showAuth) {
      ShowWindow(authHostWindow_, SW_SHOWNOACTIVATE);
      SetWindowPos(authHostWindow_, HWND_BOTTOM, bounds_.left, bounds_.top, width, height,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
      ShowWindow(authHostWindow_, SW_HIDE);
    }
  }
  backgroundHostPlaced_ = !viewVisible_;
  controllerLayoutValid_ = true;
  lastControllerLayoutBounds_ = bounds_;
  lastControllerLayoutTab_ = selectedTab_;
  lastLayoutHadAuthController_ = authController_ != nullptr;
  std::lock_guard lock(mutex_);
  status_.visible = viewVisible_;
}

}  // namespace hp
