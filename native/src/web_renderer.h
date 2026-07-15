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
  StationheadAudioToggle,
  StationheadAudioMute
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
  std::vector<StationheadPlayHistorySample> stationheadPlayHistory;
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

struct NativePlaybackFeedStatus {
  bool available = false;
  bool playing = false;
  bool hasTrack = false;
  bool endedWithoutNextTrack = false;
  uint64_t contentRevision = 0;
};

struct NativeMinuteFactsProjection {
  bool available = false;
  bool ok = false;
  bool stale = false;
  bool isBroadcasting = false;
  bool isPaused = false;
  int listenerCount = 0;
  int onlineMemberCount = 0;
  int64_t minuteAt = 0;
  int64_t fetchedAt = 0;
};

inline constexpr int kRadarCanvasWidth = 1920;
inline constexpr int kRadarCanvasHeight = 1280;
inline constexpr COLORREF kNativeDashboardBackground = RGB(7, 10, 16);

struct NativeDashboardLayout {
  RECT side{};
  RECT radar{};
  RECT main{};
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

inline NativeDashboardLayout ComputeNativeDashboardLayout(const RECT& bounds) {
  const int clientWidth = std::max(1L, bounds.right - bounds.left);
  const int clientHeight = std::max(1L, bounds.bottom - bounds.top);
  const int marginX = clientWidth * 14 / 1000;
  const int marginY = clientHeight * 20 / 1000;
  const int gapX = std::max(6, clientWidth * 11 / 1000);
  const int gapY = std::max(6, clientHeight * 18 / 1000);

  const RECT inner{bounds.left + marginX, bounds.top + marginY,
                   bounds.right - marginX, bounds.bottom - marginY};
  const int innerWidth = std::max(1L, inner.right - inner.left);
  const int innerHeight = std::max(1L, inner.bottom - inner.top);
  const int sideWidth = innerWidth * 285 / 1000;
  const int radarHeight = innerHeight * 600 / 1000;

  NativeDashboardLayout layout;
  layout.side = RECT{inner.left, inner.top, inner.left + sideWidth, inner.bottom};
  layout.radar = RECT{inner.left + sideWidth + gapX, inner.top, inner.right,
                      inner.top + radarHeight};
  layout.main = RECT{inner.left + sideWidth + gapX, inner.top + radarHeight + gapY,
                     inner.right, inner.bottom};
  if (layout.radar.right <= layout.radar.left) layout.radar.right = layout.radar.left + 1;
  if (layout.main.right <= layout.main.left) layout.main.right = layout.main.left + 1;
  if (layout.main.bottom <= layout.main.top) layout.main.bottom = layout.main.top + 1;
  return layout;
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
  void TickNativePanels(int64_t nowMs, bool timerDriven = false);
  NativePlaybackFeedStatus NativePlaybackFeedStatusFor(size_t source, int64_t nowMs) const;
  NativeMinuteFactsProjection NativeMinuteFactsSnapshot() const;
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
    uint64_t contentRevision = 0;
    bool hasPayload = false;
  };
  struct NativePlaybackTickState {
    bool active = false;
    size_t source = 0;
    size_t trackIndex = 0;
    uint64_t contentRevision = 0;

    bool operator==(const NativePlaybackTickState&) const = default;
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

  struct NativePanelPaintScope {
    NativePanelPaintScope(Renderer& renderer, HWND hwnd);
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
    RECT dirty{};
  };

  bool EnsureNativeStaticWindows();
  void ApplyNativeStaticBounds();
  void DestroyNativeStaticWindows();
  void UpdateNativeStaticPanels(const RenderState& state);

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

  enum class FontTier { Small, Medium, Large };
  HFONT TierFont(FontTier tier) const;
  enum class PanelSection {
    Clock,
    ClockTime,
    PlaybackProgress,
    Air,
    Weather,
    Controls,
    Music,
    Energy,
    News
  };
  void InvalidatePanelSection(HWND window, PanelSection section);
  void PaintNativeSide(HWND hwnd);
  void PaintNativeMain(HWND hwnd);
  void PaintNativeRadar(HWND hwnd);
  void DrawClockSection(HDC dc, const RECT& card);
  void DrawAirSection(HDC dc, const RECT& card);
  void DrawWeatherSection(HDC dc, const RECT& card);
  void DrawControlsSection(HDC dc, const RECT& card);
  void DrawMusicSection(HDC dc, const RECT& card);
  void DrawEnergySection(HDC dc, const RECT& card);
  void DrawNewsSection(HDC dc, const RECT& card);
  HBITMAP NativePanelBackBuffer(HWND hwnd, HDC dc, int width, int height);
  void ReleaseNativePanelBackBuffer(HWND hwnd);
  void QueueAction(UiAction action);
  void StartNativePlaybackBridge();
  void StopNativePlaybackBridge() noexcept;
  void NativePlaybackLoop();
  void StartNativeMinuteFactsBridge();
  void StopNativeMinuteFactsBridge() noexcept;
  void NativeMinuteFactsLoop();
  NativePlaybackRender ResolveNativePlaybackLocked(size_t source, int64_t nowMs) const;
  NativePlaybackRender ResolveNativePlayback(size_t source, int64_t nowMs) const;
  NativePlaybackTickState NativePlaybackTickStateFor(int64_t nowMs) const;
  HBITMAP NativeArtworkBitmap(const std::wstring& url, int width, int height);
  HBITMAP NativeWeatherIconBitmap(const std::wstring& icon, bool night, int width, int height);
  HBITMAP CacheNativeBitmap(const std::wstring& key, HBITMAP bitmap);
  void StartRadarCompose();
  void StopRadarCompose() noexcept;
  void RadarComposeLoop();
  void ComposeRadarFrame();
  void InvalidateAllNativePanels();
  RECT ClientBounds() const;

  struct NativePanelSlot {
    HWND Renderer::* window;
    RECT NativeDashboardLayout::* rect;
    const wchar_t* title;
    int id;
  };
  static const std::array<NativePanelSlot, 3>& NativePanelSlots();

  HWND window_{};
  HWND nativeSideWindow_{};
  HWND nativeMainWindow_{};
  HWND nativeRadarWindow_{};
  SensorSnapshot nativeSensors_{};
  std::vector<AirHistorySample> nativeAirHistory_;
  std::vector<StationheadPlayHistorySample> nativeStationheadPlayHistory_;
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
  bool nativePanelTimerActive_ = false;
  int nativeClockDayKey_ = 0;
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
  uint64_t nativePlaybackContentRevision_ = 0;
  NativePlaybackTickState nativePlaybackTickState_{};
  std::map<std::wstring, ArtworkBitmapCacheEntry> nativeArtworkBitmaps_;
  uint64_t nativeArtworkUseCounter_ = 0;
  std::map<HWND, NativeBackBuffer> nativeBackBuffers_;
  std::atomic<uint64_t> nativePlaybackRevision_{0};
  std::atomic<bool> nativePlaybackStarted_{false};
  std::atomic<bool> nativePlaybackStopping_{false};
  std::thread nativeMinuteFactsThread_;
  std::condition_variable nativeMinuteFactsWake_;
  std::mutex nativeMinuteFactsWakeMutex_;
  mutable std::mutex nativeMinuteFactsMutex_;
  NativeMinuteFactsProjection nativeMinuteFacts_{};
  std::atomic<bool> nativeMinuteFactsStarted_{false};
  std::atomic<bool> nativeMinuteFactsStopping_{false};
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
}