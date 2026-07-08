#include "web_renderer.h"

namespace hp {
namespace {
void PrepareParentWindow(HWND window) {
  SetClassLongPtrW(window, GCLP_HBRBACKGROUND,
                   reinterpret_cast<LONG_PTR>(GetStockObject(BLACK_BRUSH)));
  const LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
  if ((style & WS_EX_NOREDIRECTIONBITMAP) != 0) {
    SetWindowLongPtrW(window, GWL_EXSTYLE, style & ~WS_EX_NOREDIRECTIONBITMAP);
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  }
}
}  // namespace

Renderer::Renderer(HWND window, int width, int height, RadarManager& radar)
    : window_(window), width_(width), height_(height), radar_(radar) {
  wchar_t executable[MAX_PATH * 4]{};
  GetModuleFileNameW(nullptr, executable, _countof(executable));
  rootDir_ = fs::path(executable).parent_path();
  dataDir_ = rootDir_ / L"data";
  uiDir_ = rootDir_ / L"ui";
  userDataDir_ = dataDir_ / L"webview2-stationhead";
  bounds_ = RECT{0, 0, width_, height_};
  // Start HTTP playback collection before the dashboard WebView is allowed to
  // start. The results are cached and flushed once the UI announces readiness.
  StartNativePlaybackBridge();
}

Renderer::~Renderer() {
  shuttingDown_ = true;
  StopNativePlaybackBridge();
  CloseWebView();
}

void Renderer::Initialize() {
  PrepareParentWindow(window_);
  StartNativePlaybackBridge();
  CreateWebView();
}

bool Renderer::EnsureDashboardHostWindow() {
  if (dashboardHost_ && IsWindow(dashboardHost_)) return true;
  if (!window_ || !IsWindow(window_)) return false;
  static constexpr wchar_t kDashboardHostClassName[] = L"HomePanelDashboardHost";
  static std::once_flag classOnce;
  std::call_once(classOnce, [] {
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kDashboardHostClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&windowClass);
  });
  const int width = std::max(1L, bounds_.right - bounds_.left);
  const int height = std::max(1L, bounds_.bottom - bounds_.top);
  dashboardHost_ = CreateWindowExW(0, kDashboardHostClassName, L"HomePanelDashboardHost",
      WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
      bounds_.left, bounds_.top, width, height, window_, nullptr,
      GetModuleHandleW(nullptr), nullptr);
  if (!dashboardHost_ || !IsWindow(dashboardHost_)) return false;
  ApplyDashboardHostBounds();
  return true;
}

void Renderer::ApplyDashboardHostBounds() {
  const int width = std::max(1L, bounds_.right - bounds_.left);
  const int height = std::max(1L, bounds_.bottom - bounds_.top);
  if (dashboardHost_ && IsWindow(dashboardHost_)) {
    UINT flags = SWP_NOACTIVATE;
    if (controllerVisible_) flags |= SWP_SHOWWINDOW;
    SetWindowPos(dashboardHost_, HWND_TOP, bounds_.left, bounds_.top, width, height, flags);
  }
  const RECT controllerBounds{0, 0, width, height};
  if (controller_ && (!controllerBoundsValid_ || !EqualRect(&appliedControllerBounds_, &controllerBounds))) {
    if (SUCCEEDED(controller_->put_Bounds(controllerBounds))) {
      appliedControllerBounds_ = controllerBounds;
      controllerBoundsValid_ = true;
    }
  }
}

void Renderer::DestroyDashboardHostWindow() {
  if (dashboardHost_ && IsWindow(dashboardHost_)) DestroyWindow(dashboardHost_);
  dashboardHost_ = nullptr;
}

void Renderer::Resize(int width, int height) {
  width_ = std::max(1, width);
  height_ = std::max(1, height);
  bounds_.right = std::max(bounds_.left + 1L, bounds_.left + width_);
  bounds_.bottom = std::max(bounds_.top + 1L, bounds_.top + height_);
  ApplyDashboardHostBounds();
}

void Renderer::SetBounds(const RECT& bounds) {
  bounds_ = bounds;
  width_ = std::max(1L, bounds.right - bounds.left);
  height_ = std::max(1L, bounds.bottom - bounds.top);
  ApplyDashboardHostBounds();
}

void Renderer::SetVisible(bool visible) {
  if (controller_ && controllerVisible_ != visible) {
    if (SUCCEEDED(controller_->put_IsVisible(visible ? TRUE : FALSE))) {
      controllerVisible_ = visible;
    }
  }
  if (dashboardHost_ && IsWindow(dashboardHost_)) {
    if (visible) ApplyDashboardHostBounds();
    else ShowWindow(dashboardHost_, SW_HIDE);
  }
}

bool Renderer::LoadDashboard(const fs::path& jsonPath, bool* changed) {
  if (changed) *changed = false;
  try {
    std::ifstream input(jsonPath, std::ios::binary);
    if (!input) return false;
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    if (text.empty()) return false;
    if (text == dashboardUtf8_) return true;
    const std::wstring wide = Utf8ToWide(text);
    ParseDashboardMetadata(wide);
    dashboardUtf8_ = text;
    dashboardJson_ = wide;
    ++dashboardSourceRevision_;
    if (changed) *changed = true;
    return true;
  } catch (...) {
    dashboardUtf8_.clear();
    dashboardJson_ = L"{}";
    newsCount_ = 0;
    monitorHostHandle_.clear();
    cloudPlayback_ = CloudPlaybackState{};
    lastResolvedPlaybackIndex_ = -2;
    ++dashboardSourceRevision_;
    return false;
  }
}

RECT Renderer::ClientBounds() const { return bounds_; }
}  // namespace hp
