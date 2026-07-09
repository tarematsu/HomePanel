#pragma once
#include "common.h"
#include "dashboard_data.h"
#include "radar.h"
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
  RadarToggle,
  RadarPrevious,
  RadarNext,
  RadarSeek,
  StationheadAudioToggleA,
  StationheadAudioToggleB
};

struct AirHistorySample {
  int64_t timestamp = 0;
  int co2 = 0;
  double temperature = 0;
  double humidity = 0;
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

struct NativeDashboardLayout {
  RECT air{};
  RECT controls{};
  RECT news{};
  RECT weather{};
  RECT energy{};
  RECT stationhead{};
  RECT radar{};
  RECT clock{};
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

// Overlay layout: the radar map fills the entire client area as a living
// background. Air/Energy/Stationhead/Weather float as translucent cards in
// the four corners, while News/Clock/Controls stack down the center: news up
// top, the clock just above the update/restart buttons, and controls pinned
// to the bottom.
inline NativeDashboardLayout ComputeNativeDashboardLayout(const RECT& bounds) {
  const int clientWidth = std::max(1L, bounds.right - bounds.left);
  const int clientHeight = std::max(1L, bounds.bottom - bounds.top);
  const int shortSide = std::min(clientWidth, clientHeight);
  const int margin = std::clamp(shortSide * 3 / 100, 16, 32);
  const int gap = std::clamp(shortSide * 2 / 100, 10, 20);

  NativeDashboardLayout layout;
  layout.radar = bounds;

  const int centerWidth = std::clamp(clientWidth * 30 / 100, 320, 480);
  const int cornerBaseHeight = std::clamp(clientHeight * 19 / 100, 150, 210) +
                               std::clamp(shortSide * 8 / 100, 40, 70);
  const int cornerWidth = std::clamp(centerWidth * 6 / 5, 384, 576);
  const int cornerHeight = std::clamp(cornerBaseHeight * 11 / 10, 209, 308);
  const int centerLeft = bounds.left + (clientWidth - centerWidth) / 2;
  const int centerRight = centerLeft + centerWidth;

  layout.air = RECT{bounds.left + margin, bounds.top + margin,
                    bounds.left + margin + cornerWidth, bounds.top + margin + cornerHeight};
  layout.energy = RECT{bounds.right - margin - cornerWidth, bounds.top + margin,
                       bounds.right - margin, bounds.top + margin + cornerHeight};
  layout.stationhead = RECT{bounds.left + margin, bounds.bottom - margin - cornerHeight,
                            bounds.left + margin + cornerWidth, bounds.bottom - margin};
  layout.weather = RECT{bounds.right - margin - cornerWidth, bounds.bottom - margin - cornerHeight,
                        bounds.right - margin, bounds.bottom - margin};

  const int newsHeight = std::clamp(clientHeight * 9 / 100, 56, 84);
  layout.news = RECT{centerLeft, bounds.top + margin, centerRight, bounds.top + margin + newsHeight};

  const int controlsHeight = std::clamp(clientHeight * 14 / 100, 100, 140);
  layout.controls = RECT{centerLeft, bounds.bottom - margin - controlsHeight,
                         centerRight, bounds.bottom - margin};

  const int clockHeight = std::clamp(clientHeight * 15 / 100, 100, 160);
  const int clockTop = layout.controls.top - gap - clockHeight;
  layout.clock = RECT{centerLeft, clockTop, centerRight, clockTop + clockHeight};

  return layout;
}

class Renderer {
 public:
  Renderer(HWND window, int width, int height, RadarManager& radar);
  ~Renderer();
  void Initialize();
  void Resize(int width, int height);
  void SetBounds(const RECT& bounds);
  void SetVisible(bool visible);
  bool IsUiReady() const noexcept { return true; }
  bool LoadDashboard(const fs::path& jsonPath, bool* changed = nullptr);
  int NewsCount() const { return newsCount_; }
  std::wstring MonitorHostHandle() const { return monitorHostHandle_; }
  void Render(const RECT& dirty, const RenderState& state);
  void UpdateState(const RenderState& state);
  void TickNativePanels(int64_t nowMs);
  void NotifyRadarUpdated();
  UiAction HitTest(POINT point, float* seekFraction = nullptr);
  RECT ClockRect() const;
  RECT SensorRect() const;
  RECT RadarRect() const;
  RECT StationheadRect() const;

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
    bool Valid() const { return paintDc != nullptr; }

    HWND hwnd = nullptr;
    PAINTSTRUCT paint{};
    HDC paintDc = nullptr;
    HDC dc = nullptr;
    HGDIOBJ previousBitmap = nullptr;
    RECT bounds{};
  };

  bool EnsureNativeClockWindow();
  void ApplyNativeClockBounds();
  void DestroyNativeClockWindow();
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
  LRESULT HandleNativeClockMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT HandleNativeStaticMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  void PaintNativeClock(HWND hwnd);
  void PaintNativeAir(HWND hwnd);
  void PaintNativeControls(HWND hwnd);
  void PaintNativeNews(HWND hwnd);
  void PaintNativeWeather(HWND hwnd);
  void PaintNativeEnergy(HWND hwnd);
  void PaintNativeStationhead(HWND hwnd);
  void PaintNativeRadar(HWND hwnd);
  HBITMAP NativePanelBackBuffer(HWND hwnd, HDC dc, int width, int height);
  void ReleaseNativePanelBackBuffer(HWND hwnd);
  void QueueAction(UiAction action, float seekFraction = 0.0f);
  void StartNativePlaybackBridge();
  void StopNativePlaybackBridge() noexcept;
  void NativePlaybackLoop();
  NativePlaybackRender ResolveNativePlaybackLocked(size_t source, int64_t nowMs) const;
  NativePlaybackRender ResolveNativePlayback(size_t source, int64_t nowMs) const;
  bool NativePlaybackActive(int64_t nowMs) const;
  HBITMAP NativeArtworkBitmap(const std::wstring& url, int width, int height);
  void StartRadarCompose();
  void StopRadarCompose() noexcept;
  void RadarComposeLoop();
  void ComposeRadarFrame();
  void InvalidateAllNativePanels();
  RECT ClientBounds() const;
  void ParseDashboardMetadata(const std::wstring& json);

  HWND window_{};
  HWND nativeClockWindow_{};
  HWND nativeAirWindow_{};
  HWND nativeControlsWindow_{};
  HWND nativeNewsWindow_{};
  HWND nativeWeatherWindow_{};
  HWND nativeEnergyWindow_{};
  HWND nativeStationheadWindow_{};
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
  RadarManager& radar_;
  fs::path rootDir_;
  fs::path dataDir_;
  std::atomic<bool> shuttingDown_{false};
  std::string dashboardUtf8_;
  uint64_t dashboardSourceRevision_ = 0;
  uint64_t spotifySourceRevision_ = 0;
  int newsCount_ = 0;
  std::wstring monitorHostHandle_;
  mutable std::mutex actionMutex_;
  UiAction pendingAction_ = UiAction::None;
  float pendingSeekFraction_ = 0.0f;
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
