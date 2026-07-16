#include "sh.h"

namespace hp {
namespace {

HWND CreateStationheadChildHost(HWND parent, const wchar_t* className, const wchar_t* title,
                                const RECT& bounds) {
  if (!parent || !IsWindow(parent)) return nullptr;
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSW registered{};
  if (!GetClassInfoW(instance, className, &registered)) {
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      return nullptr;
    }
  }

  const int width = std::max(1L, bounds.right - bounds.left);
  const int height = std::max(1L, bounds.bottom - bounds.top);
  return CreateWindowExW(0, className, title, WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                         bounds.left, bounds.top, width, height, parent, nullptr,
                         instance, nullptr);
}

bool WindowClientSizeMatches(HWND window, int width, int height) noexcept {
  RECT client{};
  return window && GetClientRect(window, &client) &&
         client.right - client.left == width &&
         client.bottom - client.top == height;
}

bool ChildWindowPlacementMatches(HWND window, const RECT& expected, HWND placement) noexcept {
  if (!window) return false;
  HWND parent = GetParent(window);
  RECT current{};
  if (!parent || !GetWindowRect(window, &current)) return false;
  POINT topLeft{current.left, current.top};
  POINT bottomRight{current.right, current.bottom};
  if (!ScreenToClient(parent, &topLeft) || !ScreenToClient(parent, &bottomRight)) return false;
  const RECT parentRelative{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
  if (!EqualRect(&parentRelative, &expected)) return false;
  if (placement == HWND_TOP) return GetWindow(window, GW_HWNDPREV) == nullptr;
  if (placement == HWND_BOTTOM) return GetWindow(window, GW_HWNDNEXT) == nullptr;
  return false;
}

bool ControllerBoundsMatch(ICoreWebView2Controller* controller,
                           const RECT& expected) noexcept {
  RECT current{};
  return controller && SUCCEEDED(controller->get_Bounds(&current)) &&
         EqualRect(&current, &expected);
}

bool ControllerVisibilityMatches(ICoreWebView2Controller* controller,
                                 BOOL expected) noexcept {
  BOOL current = FALSE;
  return controller && SUCCEEDED(controller->get_IsVisible(&current)) &&
         current == expected;
}

void ApplyStationheadChildLayout(HWND hostWindow,
                                 HWND authHostWindow,
                                 ICoreWebView2Controller* controller,
                                 ICoreWebView2Controller* authController,
                                 const RECT& bounds,
                                 bool contentVisible,
                                 bool showAuth,
                                 bool previewVisible) {
  const int width = std::max(1L, bounds.right - bounds.left);
  const int height = std::max(1L, bounds.bottom - bounds.top);
  const bool fullContent = previewVisible || contentVisible;
  const int hostWidth = fullContent ? width : 1;
  const int hostHeight = fullContent ? height : 1;
  const RECT contentBounds{0, 0, hostWidth, hostHeight};
  const RECT authBounds{0, 0, width, height};
  const RECT hostBounds{bounds.left, bounds.top, bounds.left + hostWidth, bounds.top + hostHeight};
  const RECT authHostBounds{bounds.left, bounds.top, bounds.left + width, bounds.top + height};
  const HWND hostPlacement = fullContent ? HWND_TOP : HWND_BOTTOM;
  const bool hostValid = hostWindow && IsWindow(hostWindow);
  const bool authHostValid = authHostWindow && IsWindow(authHostWindow);
  const bool hostWasVisible = hostValid && IsWindowVisible(hostWindow);
  const bool authWasVisible = authHostValid && IsWindowVisible(authHostWindow);

  if (controller) {
    if (showAuth) {
      if (!ControllerVisibilityMatches(controller, FALSE)) {
        controller->put_IsVisible(FALSE);
      }
    } else {
      if (!ControllerBoundsMatch(controller, contentBounds) ||
          !WindowClientSizeMatches(hostWindow, hostWidth, hostHeight)) {
        controller->put_Bounds(contentBounds);
      }
      if (!ControllerVisibilityMatches(controller, TRUE)) {
        controller->put_IsVisible(TRUE);
      }
    }
  }

  if (hostValid) {
    if (showAuth) {
      if (hostWasVisible) ShowWindow(hostWindow, SW_HIDE);
    } else if (!hostWasVisible ||
               !ChildWindowPlacementMatches(hostWindow, hostBounds, hostPlacement)) {
      SetWindowPos(hostWindow, hostPlacement,
                   bounds.left, bounds.top, hostWidth, hostHeight,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOSENDCHANGING);
    }
  }

  if (authController) {
    if (showAuth) {
      if (!ControllerBoundsMatch(authController, authBounds) ||
          !WindowClientSizeMatches(authHostWindow, width, height)) {
        authController->put_Bounds(authBounds);
      }
      if (!ControllerVisibilityMatches(authController, TRUE)) {
        authController->put_IsVisible(TRUE);
      }
    } else if (!ControllerVisibilityMatches(authController, FALSE)) {
      authController->put_IsVisible(FALSE);
    }
  }

  if (authHostValid) {
    if (showAuth) {
      if (!authWasVisible ||
          !ChildWindowPlacementMatches(authHostWindow, authHostBounds, HWND_TOP)) {
        SetWindowPos(authHostWindow, HWND_TOP, bounds.left, bounds.top, width, height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOSENDCHANGING);
      }
    } else if (authWasVisible) {
      ShowWindow(authHostWindow, SW_HIDE);
    }
  }
}

}

bool StationheadPlayer::EnsureHostWindow() {
  if (hostWindow_ && IsWindow(hostWindow_)) return true;
  hostWindow_ = IsSecondary()
      ? CreateStationheadChildHost(window_, L"HomePanelSecondaryStationheadHost",
                                   L"SecondaryStationheadHost", bounds_)
      : CreateStationheadChildHost(window_, L"HomePanelStationheadHost",
                                   L"StationheadHost", bounds_);
  return hostWindow_ && IsWindow(hostWindow_);
}

bool StationheadPlayer::EnsureAuthHostWindow() {
  if (authHostWindow_ && IsWindow(authHostWindow_)) return true;
  authHostWindow_ = IsSecondary()
      ? CreateStationheadChildHost(window_, L"HomePanelSecondarySpotifyAuthHost",
                                   L"SecondarySpotifyAuthHost", bounds_)
      : CreateStationheadChildHost(window_, L"HomePanelSpotifyAuthHost",
                                   L"SpotifyAuthHost", bounds_);
  return authHostWindow_ && IsWindow(authHostWindow_);
}

void StationheadPlayer::KeepPlaybackBehindDashboard() {
  if (spotifyAuthorization_ || loginRequired_) return;
  if (!startupPreviewActive_ && !viewVisible_ && selectedTab_ == StationheadTabKind::None) {
    std::lock_guard lock(mutex_);
    if (!status_.visible) return;
  }
  if (!EnsureHostWindow()) {
    viewVisible_ = false;
    std::lock_guard lock(mutex_);
    status_.visible = false;
    return;
  }
  startupPreviewActive_ = false;
  viewVisible_ = false;
  selectedTab_ = StationheadTabKind::None;
  ApplyStationheadChildLayout(hostWindow_, authHostWindow_, controller_.Get(), authController_.Get(),
                              bounds_, false, false, false);
  std::lock_guard lock(mutex_);
  status_.visible = false;
}

void StationheadPlayer::SetStartupBounds() {
  selectedTab_ = StationheadTabKind::None;
  viewVisible_ = false;
  LayoutControllers();
}

void StationheadPlayer::SetStartupPreviewBounds(const RECT& bounds) {
  startupPreviewActive_ = true;
  bounds_ = bounds;
  LayoutControllers();
}

void StationheadPlayer::ClearStartupPreviewBounds() {
  if (!startupPreviewActive_) return;
  startupPreviewActive_ = false;
  SetStartupBounds();
}

void StationheadPlayer::SetVisible(bool visible) {
  if (!visible) {
    selectedTab_ = StationheadTabKind::None;
    if (controller_) KeepPlaybackBehindDashboard();
    else {
      startupPreviewActive_ = false;
      viewVisible_ = false;
      std::lock_guard lock(mutex_);
      status_.visible = false;
    }
    if (window_ && IsWindow(window_)) SetFocus(window_);
    return;
  }
  startupPreviewActive_ = false;
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
  if (!wasVisible && controller_) controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

void StationheadPlayer::LayoutControllers() {
  if (!EnsureHostWindow()) return;
  const bool preview = startupPreviewActive_;
  const bool showAuth = !preview && selectedTab_ == StationheadTabKind::Auth;
  ApplyStationheadChildLayout(hostWindow_, authHostWindow_, controller_.Get(), authController_.Get(),
                              bounds_, viewVisible_, showAuth, preview);
  std::lock_guard lock(mutex_);
  status_.visible = preview || viewVisible_;
}

void StationheadPlayer::SetBounds(const RECT& bounds) {
  if (EqualRect(&bounds_, &bounds)) return;
  bounds_ = bounds;
  if (startupPreviewActive_ || viewVisible_ || NeedsInteractiveWindow()) LayoutControllers();
  else KeepPlaybackBehindDashboard();
}

void StationheadPlayer::SelectTab(StationheadTabKind tab) {
  if (tab == StationheadTabKind::None && NeedsInteractiveWindow()) {
    tab = spotifyAuthorization_ ? StationheadTabKind::Auth : StationheadTabKind::Stationhead;
  }
  if (selectedTab_ == tab) {
    if (tab == StationheadTabKind::None && !viewVisible_) return;
    SetVisible(tab != StationheadTabKind::None);
    return;
  }
  selectedTab_ = tab;
  SetVisible(tab != StationheadTabKind::None);
}

bool StationheadPlayer::HasAuthTab() const {
  return authController_ != nullptr || !authPendingUrl_.empty();
}

HWND StationheadPlayer::ActiveHostWindowForAccountSetup() const noexcept {
  if (selectedTab_ == StationheadTabKind::Auth && authHostWindow_ && IsWindow(authHostWindow_)) {
    return authHostWindow_;
  }
  return hostWindow_;
}

bool StationheadPlayer::NeedsInteractiveWindow() const {
  return selectedTab_ == StationheadTabKind::Auth ||
         spotifyAuthorization_ ||
         loginRequired_ ||
         (controller_ && !audioPlaying_.load(std::memory_order_relaxed));
}

}
