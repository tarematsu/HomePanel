// Renderer lifecycle and shared state: construction, window visibility,
// dashboard cache loading/metadata, queued UI actions, and the main-window
// background paint. Compiled as part of renderer_core.cpp's translation unit.
#include "web_renderer.h"

namespace hp {
bool InstallRuntimeAssets() noexcept;

namespace {
void PrepareParentWindow(HWND window) {
  static HBRUSH background = CreateSolidBrush(kNativeDashboardBackground);
  SetClassLongPtrW(window, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(background));
  const LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
  if ((style & WS_EX_NOREDIRECTIONBITMAP) != 0) {
    SetWindowLongPtrW(window, GWL_EXSTYLE, style & ~WS_EX_NOREDIRECTIONBITMAP);
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
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
  StartRadarCompose();
  ApplyNativeStaticBounds();
}

void Renderer::Resize(int width, int height) {
  width_ = std::max(1, width);
  height_ = std::max(1, height);
  bounds_.right = std::max(bounds_.left + 1L, bounds_.left + width_);
  bounds_.bottom = std::max(bounds_.top + 1L, bounds_.top + height_);
  ApplyNativeStaticBounds();
}

void Renderer::SetBounds(const RECT& bounds) {
  bounds_ = bounds;
  width_ = std::max(1L, bounds.right - bounds.left);
  height_ = std::max(1L, bounds.bottom - bounds.top);
  ApplyNativeStaticBounds();
}

void Renderer::SetVisible(bool visible) {
  nativeDashboardVisible_ = visible;
  for (const NativePanelSlot& slot : NativePanelSlots()) {
    const HWND hwnd = this->*slot.window;
    if (hwnd && IsWindow(hwnd)) ShowWindow(hwnd, visible ? SW_SHOWNA : SW_HIDE);
  }
  if (visible) ApplyNativeStaticBounds();
}

bool Renderer::LoadDashboard(const fs::path& jsonPath, bool* changed) {
  if (changed) *changed = false;
  try {
    std::ifstream input(jsonPath, std::ios::binary);
    if (!input) return false;
    std::string text((std::istreambuf_iterator<char>(input)), {});
    if (text.empty()) return false;
    if (text == dashboardUtf8_) return true;
    DashboardSnapshot snapshot;
    if (!ParseDashboardSnapshot(text, snapshot)) return false;
    newsCount_ = snapshot.newsItemCount;
    nativeDashboard_ = std::move(snapshot);
    dashboardUtf8_ = std::move(text);
    ++dashboardSourceRevision_;
    if (changed) *changed = true;
    return true;
  } catch (...) {
    dashboardUtf8_.clear();
    newsCount_ = 0;
    ++dashboardSourceRevision_;
    return false;
  }
}

RECT Renderer::ClientBounds() const { return bounds_; }

void Renderer::QueueAction(UiAction action) {
  {
    std::lock_guard lock(actionMutex_);
    pendingAction_ = action;
  }
  PostMessageW(window_, WM_LBUTTONUP, 0, MAKELPARAM(0, 0));
}

UiAction Renderer::HitTest(POINT) {
  std::lock_guard lock(actionMutex_);
  const UiAction action = pendingAction_;
  pendingAction_ = UiAction::None;
  return action;
}

void Renderer::UpdateState(const RenderState& state) {
  UpdateNativeStaticPanels(state);
}

void Renderer::Render(const RECT& dirty, const RenderState& state) {
  (void)dirty;
  (void)state;
  if (!window_ || !nativeDashboardVisible_) return;
  HDC dc = GetDC(window_);
  if (!dc) return;
  RECT bounds{};
  GetClientRect(window_, &bounds);
  HBRUSH background = CreateSolidBrush(kNativeDashboardBackground);
  FillRect(dc, &bounds, background);
  DeleteObject(background);
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
