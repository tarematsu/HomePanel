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

struct StationheadSurfacePolicy {
  bool showAuth = false;
  bool showStartupPreview = false;
};

constexpr StationheadSurfacePolicy ResolveStationheadSurfacePolicy(
    bool startupPreviewActive,
    StationheadTabKind selectedTab,
    bool authSurfaceReady) noexcept {
  const bool showAuth = selectedTab == StationheadTabKind::Auth && authSurfaceReady;
  return {showAuth, startupPreviewActive && !showAuth};
}

static_assert(ResolveStationheadSurfacePolicy(
                  true, StationheadTabKind::None, false).showStartupPreview);
static_assert(ResolveStationheadSurfacePolicy(
                  true, StationheadTabKind::Auth, true).showAuth);
static_assert(!ResolveStationheadSurfacePolicy(
                   true, StationheadTabKind::Auth, true).showStartupPreview);
static_assert(!ResolveStationheadSurfacePolicy(
                   true, StationheadTabKind::Auth, false).showAuth);
static_assert(ResolveStationheadSurfacePolicy(
                  true, StationheadTabKind::Auth, false).showStartupPreview);
static_assert(ResolveStationheadSurfacePolicy(
                  false, StationheadTabKind::Auth, true).showAuth);

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
  if (spotifyAuthorization_ || loginRequired_) {
    selectedTab_ = spotifyAuthorization_
        ? StationheadTabKind::Auth
        : StationheadTabKind::Stationhead;
    viewVisible_ = true;
    LayoutControllers();
    return;
  }
  if (startupPreviewActive_) {
    // The App owns the startup-preview lifetime. A player can request to hide
    // after audio starts or auth completes, but clearing only this local flag
    // desynchronizes it from the A/B handle and leaves a blank half-screen.
    // Keep the preview surface until App::ClearStartupStationheadPreview()
    // releases both players together.
    viewVisible_ = false;
    selectedTab_ = StationheadTabKind::None;
    LayoutControllers();
    return;
  }
  if (!viewVisible_ && selectedTab_ == StationheadTabKind::None) {
    std::lock_guard lock(mutex_);
    if (!status_.visible) return;
  }
  if (!EnsureHostWindow()) {
    viewVisible_ = false;
    std::lock_guard lock(mutex_);
    status_.visible = false;
    return;
  }
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
  const bool preserveInteractiveTab =
      selectedTab_ != StationheadTabKind::None && NeedsInteractiveWindow();
  startupPreviewActive_ = false;
  if (preserveInteractiveTab) {
    viewVisible_ = true;
    LayoutControllers();
    return;
  }
  SetStartupBounds();
}

void StationheadPlayer::SetVisible(bool visible) {
  if (!visible) {
    selectedTab_ = StationheadTabKind::None;
    if (controller_) KeepPlaybackBehindDashboard();
    else {
      viewVisible_ = false;
      std::lock_guard lock(mutex_);
      status_.visible = startupPreviewActive_;
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
    status_.visible = startupPreviewActive_ || viewVisible_;
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
  const bool authSurfaceReady = authController_ && authWebview_;
  const StationheadSurfacePolicy policy = ResolveStationheadSurfacePolicy(
      startupPreviewActive_, selectedTab_, authSurfaceReady);
  ApplyStationheadChildLayout(hostWindow_, authHostWindow_, controller_.Get(), authController_.Get(),
                              bounds_, viewVisible_, policy.showAuth,
                              policy.showStartupPreview);
  std::lock_guard lock(mutex_);
  status_.visible = policy.showStartupPreview || policy.showAuth || viewVisible_;
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
  if (selectedTab_ == StationheadTabKind::Auth && authController_ && authWebview_ &&
      authHostWindow_ && IsWindow(authHostWindow_)) {
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

}  // namespace hp