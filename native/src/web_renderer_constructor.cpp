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
}

Renderer::~Renderer() {
  shuttingDown_ = true;
  StopNativePlaybackBridge();
  StopRadarCompose();
  DestroyNativeStaticWindows();
  DestroyNativeClockWindow();
}

void Renderer::Initialize() {
  PrepareParentWindow(window_);
  EnsureNativeClockWindow();
  EnsureNativeStaticWindows();
  StartNativePlaybackBridge();
  StartRadarCompose();
  ApplyNativeClockBounds();
  ApplyNativeStaticBounds();
}

void Renderer::Resize(int width, int height) {
  width_ = std::max(1, width);
  height_ = std::max(1, height);
  bounds_.right = std::max(bounds_.left + 1L, bounds_.left + width_);
  bounds_.bottom = std::max(bounds_.top + 1L, bounds_.top + height_);
  ApplyNativeClockBounds();
  ApplyNativeStaticBounds();
}

void Renderer::SetBounds(const RECT& bounds) {
  bounds_ = bounds;
  width_ = std::max(1L, bounds.right - bounds.left);
  height_ = std::max(1L, bounds.bottom - bounds.top);
  ApplyNativeClockBounds();
  ApplyNativeStaticBounds();
}

void Renderer::SetVisible(bool visible) {
  nativeDashboardVisible_ = visible;
  if (nativeClockWindow_ && IsWindow(nativeClockWindow_)) {
    ShowWindow(nativeClockWindow_, visible ? SW_SHOWNA : SW_HIDE);
    if (visible) ApplyNativeClockBounds();
  }
  if (nativeAirWindow_ && IsWindow(nativeAirWindow_)) {
    ShowWindow(nativeAirWindow_, visible ? SW_SHOWNA : SW_HIDE);
  }
  if (nativeAirHistoryWindow_ && IsWindow(nativeAirHistoryWindow_)) {
    ShowWindow(nativeAirHistoryWindow_, visible ? SW_SHOWNA : SW_HIDE);
  }
  if (nativeControlsWindow_ && IsWindow(nativeControlsWindow_)) {
    ShowWindow(nativeControlsWindow_, visible ? SW_SHOWNA : SW_HIDE);
  }
  if (nativeNewsWindow_ && IsWindow(nativeNewsWindow_)) {
    ShowWindow(nativeNewsWindow_, visible ? SW_SHOWNA : SW_HIDE);
  }
  if (nativeWeatherWindow_ && IsWindow(nativeWeatherWindow_)) {
    ShowWindow(nativeWeatherWindow_, visible ? SW_SHOWNA : SW_HIDE);
  }
  if (nativeEnergyWindow_ && IsWindow(nativeEnergyWindow_)) {
    ShowWindow(nativeEnergyWindow_, visible ? SW_SHOWNA : SW_HIDE);
  }
  if (nativeStationheadWindow_ && IsWindow(nativeStationheadWindow_)) {
    ShowWindow(nativeStationheadWindow_, visible ? SW_SHOWNA : SW_HIDE);
  }
  if (nativeRadarWindow_ && IsWindow(nativeRadarWindow_)) {
    ShowWindow(nativeRadarWindow_, visible ? SW_SHOWNA : SW_HIDE);
  }
  if (visible) ApplyNativeStaticBounds();
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
    DashboardSnapshot snapshot;
    LoadDashboardSnapshot(jsonPath, snapshot);
    nativeDashboard_ = std::move(snapshot);
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
    ++dashboardSourceRevision_;
    return false;
  }
}

RECT Renderer::ClientBounds() const { return bounds_; }
}  // namespace hp
