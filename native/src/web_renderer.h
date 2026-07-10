#pragma once
#include "common.h"
#include "dashboard_data.h"
#include "sensors.h"
#include "sh.h"

namespace hp {
enum class UiAction {
  None,
  WorkspaceMain,
  WorkspaceAuth,
  DataRefresh,
  AppUpdate,
  Restart,
  Maintenance,
  StationheadReconnect,
  ClearCache,
  ShowLog,
  CloseMaintenance,
  StationheadAudioToggleA,
  StationheadAudioToggleB
};

struct AirHistorySample {
  int64_t timestamp = 0;
  int co2 = 0;
  double temperature = 0;
  double humidity = 0;

  bool operator==(const AirHistorySample&) const = default;
};

struct RenderState {
  SensorSnapshot sensors;
  StationheadStatus stationhead;
  std::wstring appVersion;
  std::vector<AirHistorySample> airHistory;
  int workspaceTab = 0;
  std::wstring toast;
  int newsIndex = 0;
  bool maintenance = false;
};

struct NativePlaybackTrack {
  std::wstring title;
  std::wstring artist;
  std::wstring artwork;
  int64_t durationMs = 0;
};

struct NativePlaybackProjection {
  bool available = false;
  bool playing = false;
  int currentIndex = 0;
  int64_t progressMs = 0;
  int64_t anchorAt = 0;
  int64_t sampledAt = 0;
  int64_t queueEndAt = 0;
  int64_t fetchedAt = 0;
  std::vector<NativePlaybackTrack> queue;
};

struct NativePlaybackRender {
  bool available = false;
  bool hasTrack = false;
  bool playing = false;
  int64_t progressMs = 0;
  NativePlaybackTrack track;
};

inline constexpr int kRadarCanvasWidth = 1920;
inline constexpr int kRadarCanvasHeight = 1280;
inline constexpr COLORREF kNativeDashboardBackground = RGB(7, 10, 16);

// Overlay layout: the radar map fills the entire client area as a living
// background. Two full-width merged panels float over it: the top panel
// (air quality | news | energy) and the bottom panel (stationhead |
// clock+controls | weather). Everything is derived proportionally from the
// client size so any resolution lays out the same way.
struct NativeDashboardLayout {
  RECT top{};
  RECT bottom{};
  RECT radar{};
};

// A merged panel is split into three horizontal sections: the former corner
// cards on the left/right and the former center strip in the middle.
struct NativePanelSections {
  RECT left{};
  RECT center{};
  RECT right{};
};

inline RECT NormalizeInsetRect(RECT rect, int left, int top, int right, int bottom) {
  rect.left += left;
  rect.top += top;
  rect.right -= right;
  rect.bottom -= bottom;
  if (rect.right <= rect.left) rect.right = rect.left + 1;
  if (rect.bottom <= rect.top) rect.bottom = rect.top + 1;
  return rect;
}

// Insets a rect by fractions (permille) of its own size, so padding scales
// with the panel instead of being an absolute pixel count.
inline RECT RelativeInsetRect(const RECT& rect, int horizontalPermille, int verticalPermille) {
  const int width = std::max(1L, rect.right - rect.left);
  const int height = std::max(1L, rect.bottom - rect.top);
  return NormalizeInsetRect(rect, width * horizontalPermille / 1000, height * verticalPermille / 1000,
                            width * horizontalPermille / 1000, height * verticalPermille / 1000);
}

inline NativeDashboardLayout ComputeNativeDashboardLayout(const RECT& bounds) {
  const int clientWidth = std::max(1L, bounds.right - bounds.left);
  const int clientHeight = std::max(1L, bounds.bottom - bounds.top);
  const int marginX = clientWidth * 2 / 100;
  const int marginY = clientHeight * 3 / 100;
  // Top and bottom panels share one height: the former 26%/31% pair, shrunk
  // by 10% and unified at the midpoint.
  const int panelHeight = clientHeight * 256 / 1000;

  NativeDashboardLayout layout;
  layout.radar = bounds;
  layout.top = RECT{bounds.left + marginX, bounds.top + marginY,
                    bounds.right - marginX, bounds.top + marginY + panelHeight};
  layout.bottom = RECT{bounds.left + marginX, bounds.bottom - marginY - panelHeight,
                       bounds.right - marginX, bounds.bottom - marginY};
  return layout;
}

// Left/right sections take 33% of the panel width each; the center strip is
// what remains after two relative gutters.
inline NativePanelSections SplitPanelSections(const RECT& panel) {
  const int width = std::max(1L, panel.right - panel.left);
  const int gutter = width * 15 / 1000;
  const int sideWidth = width * 33 / 100;
  NativePanelSections sections;
  sections.left = RECT{panel.left, panel.top, panel.left + sideWidth, panel.bottom};
  sections.right = RECT{panel.right - sideWidth, panel.top, panel.right, panel.bottom};
  sections.center = RECT{sections.left.right + gutter, panel.top,
                         sections.right.left - gutter, panel.bottom};
  if (sections.center.right <= sections.center.left) {
    sections.center.right = sections.center.left + 1;
  }
  return sections;
}

class Renderer {
 public:
  Renderer(HWND window, int width, int height);
  ~Renderer();
  void Initialize();
  void Resize(int width, int height);
  void SetBounds(const RECT& bounds);
  void SetVisible(bool visible);
  bool LoadDashboard(const fs::path& jsonPath, bool* changed = nullptr);
  int NewsCount() const { return newsCount_; }
  void Render(const RECT& dirty, const RenderState& state);
  void UpdateState(const RenderState& state);
  void TickNativePanels(int64_t nowMs);
  void NotifyRadarUpdated();
  UiAction HitTest(POINT point);

 private:
  struct NativePlaybackUpdate {
    std::wstring source;
    std::wstring payload;
    NativePlaybackProjection projection;
    std::wstring error;
    int64_t fetchedAt = 0;
    uint64_t revision = 0;
    bool hasPayload = false;
  };
  struct ArtworkBitmapCacheEntry {
    HBITMAP bitmap = nullptr;
    uint64_t lastUsed = 0;
  };
  struct NativeBackBuffer {
    HBITMAP bitmap = nullptr;
    int width = 0;
    int height = 0;
  };
  // Shared BeginPaint/back-buffer setup and BitBlt/EndPaint teardown for every
  // PaintNativeXxx panel; each panel only supplies the drawing in between.
  // The back layer is always sampled from the live radar frame at this
  // panel's absoluteRect (its position within the overall dashboard), so
  // every floating panel reads as a window looking through to the map
  // behind it. Passing tintAlpha > 0 blends a dark frosted-glass tint over
  // a cornerRadius-rounded region on top of that sample; tintAlpha == 0
  // (used by the radar panel itself) leaves the sampled map untouched.
  struct NativePanelPaintScope {
    NativePanelPaintScope(Renderer& renderer, HWND hwnd, const RECT& absoluteRect,
                         BYTE tintAlpha = 168, int cornerRadius = 22,
                         COLORREF tintColor = RGB(10, 14, 22));
    ~NativePanelPaintScope();
    NativePanelPaintScope(const NativePanelPaintScope&) = delete;
    NativePanelPaintScope& operator=(const NativePanelPaintScope&) = delete;
    bool Valid() const { return paintDc != nullptr && dc != nullptr; }

    HWND hwnd = nullptr;
    PAINTSTRUCT paint{};
    HDC paintDc = nullptr;
    HDC dc = nullptr;
    HGDIOBJ previousBitmap = nullptr;
    RECT bounds{};
    // Region actually invalidated this paint. Sampling/tinting/drawing are
    // clipped to it and the destructor blits only this rect, so a 1s clock
    // tick repaints just the clock section instead of the whole panel.
    RECT dirty{};
  };

  bool EnsureNativeStaticWindows();
  void ApplyNativeStaticBounds();
  void DestroyNativeStaticWindows();
  void UpdateNativeStaticPanels(const RenderState& state);
  // Shared WM_NCCREATE/GWLP_USERDATA thunk for the native panel window
  // classes; Handler picks which instance method a given class dispatches to.
  template <LRESULT (Renderer::*Handler)(HWND, UINT, WPARAM, LPARAM)>
  static LRESULT CALLBACK NativeWndProcThunk(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    Renderer* renderer = reinterpret_cast<Renderer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      auto* createstruct = reinterpret_cast<CREATESTRUCTW*>(lparam);
      renderer = reinterpret_cast<Renderer*>(createstruct->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(renderer));
    }
    if (renderer) return (renderer->*Handler)(hwnd, message, wparam, lparam);
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }
  LRESULT HandleNativeStaticMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  // Unified typography: every label picks one of exactly three sizes, all
  // derived from the dashboard height.
  enum class FontTier { Small, Medium, Large };
  HFONT TierFont(FontTier tier) const;
  enum class PanelSection { Left, Center, Right };
  void InvalidatePanelSection(HWND window, PanelSection section);
  void PaintNativeTop(HWND hwnd);
  void PaintNativeBottom(HWND hwnd);
  void PaintNativeRadar(HWND hwnd);
  void DrawAirSection(HDC dc, const RECT& section);
  void DrawNewsSection(HDC dc, const RECT& section);
  void DrawEnergySection(HDC dc, const RECT& section);
  void DrawStationheadSection(HDC dc, const RECT& section);
  void DrawClockControlsSection(HDC dc, const RECT& section);
  void DrawWeatherSection(HDC dc, const RECT& section);
  HBITMAP NativePanelBackBuffer(HWND hwnd, HDC dc, int width, int height);
  void ReleaseNativePanelBackBuffer(HWND hwnd);
  void QueueAction(UiAction action);
  void StartNativePlaybackBridge();
  void StopNativePlaybackBridge() noexcept;
  void NativePlaybackLoop();
  NativePlaybackRender ResolveNativePlaybackLocked(size_t source, int64_t nowMs) const;
  NativePlaybackRender ResolveNativePlayback(size_t source, int64_t nowMs) const;
  bool NativePlaybackActive(int64_t nowMs) const;
  HBITMAP NativeArtworkBitmap(const std::wstring& url, int width, int height);
  HBITMAP NativeWeatherIconBitmap(const std::wstring& icon, bool night, int width, int height);
  HBITMAP CacheNativeBitmap(const std::wstring& key, HBITMAP bitmap);
  void StartRadarCompose();
  void StopRadarCompose() noexcept;
  void RadarComposeLoop();
  void ComposeRadarFrame();
  void InvalidateAllNativePanels();
  RECT ClientBounds() const;

  // One entry per native panel window. Radar comes first so placement loops
  // stack the merged top/bottom panels above the full-screen radar background.
  struct NativePanelSlot {
    HWND Renderer::* window;
    RECT NativeDashboardLayout::* rect;
    const wchar_t* title;
    int id;
  };
  static const std::array<NativePanelSlot, 3>& NativePanelSlots();

  HWND window_{};
  HWND nativeTopWindow_{};
  HWND nativeBottomWindow_{};
  HWND nativeRadarWindow_{};
  SensorSnapshot nativeSensors_{};
  std::vector<AirHistorySample> nativeAirHistory_;
  StationheadStatus nativeStationhead_{};
  DashboardSnapshot nativeDashboard_{};
  std::wstring nativeAppVersion_;
  std::wstring nativeToast_;
  int nativeNewsIndex_ = 0;
  uint64_t nativeRenderedDashboardRevision_ = 0;
  int width_ = 0;
  int height_ = 0;
  RECT bounds_{};
  bool nativeDashboardVisible_ = true;
  fs::path rootDir_;
  fs::path dataDir_;
  std::atomic<bool> shuttingDown_{false};
  std::string dashboardUtf8_;
  uint64_t dashboardSourceRevision_ = 0;
  uint64_t spotifySourceRevision_ = 0;
  int newsCount_ = 0;
  mutable std::mutex actionMutex_;
  UiAction pendingAction_ = UiAction::None;
  std::thread nativePlaybackThread_;
  std::condition_variable nativePlaybackWake_;
  std::mutex nativePlaybackWakeMutex_;
  mutable std::mutex nativePlaybackMutex_;
  std::array<NativePlaybackUpdate, 2> nativePlaybackUpdates_{};
  std::map<std::wstring, ArtworkBitmapCacheEntry> nativeArtworkBitmaps_;
  uint64_t nativeArtworkUseCounter_ = 0;
  std::map<HWND, NativeBackBuffer> nativeBackBuffers_;
  std::atomic<uint64_t> nativePlaybackRevision_{0};
  std::atomic<bool> nativePlaybackStarted_{false};
  std::atomic<bool> nativePlaybackStopping_{false};
  std::thread radarComposeThread_;
  std::condition_variable radarComposeWake_;
  std::mutex radarComposeWakeMutex_;
  mutable std::mutex radarFrameMutex_;
  HBITMAP radarFrameBitmap_ = nullptr;
  std::wstring radarTimeText_ = L"--:--";
  std::wstring radarSignature_;
  std::map<std::wstring, int64_t> radarFailedTiles_;
  bool radarComposePending_ = false;
  std::atomic<bool> radarComposeStarted_{false};
  std::atomic<bool> radarComposeStopping_{false};
};
}  // namespace hp
