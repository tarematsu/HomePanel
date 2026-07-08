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

inline constexpr int kRadarCanvasWidth = 800;
inline constexpr int kRadarCanvasHeight = 520;

struct NativeDashboardLayout {
  RECT air{};
  RECT airHistory{};
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

inline NativeDashboardLayout ComputeNativeDashboardLayout(const RECT& bounds) {
  const int clientWidth = std::max(1L, bounds.right - bounds.left);
  const int clientHeight = std::max(1L, bounds.bottom - bounds.top);
  const int margin = clientWidth >= 1200 && clientHeight >= 700 ? 12 : 6;
  const int gap = clientWidth >= 1200 && clientHeight >= 700 ? 8 : 4;
  const int gridWidth = std::max(1, clientWidth - margin * 2);
  const int gridHeight = std::max(1, clientHeight - margin * 2);
  const int columnWidth = std::max(1, (gridWidth - gap * 2) / 3);
  const int rowHeight = std::max(1, (gridHeight - gap) / 2);
  const int left = bounds.left + margin;
  const int top = bounds.top + margin;
  const auto panel = [&](int column, int row) {
    const int panelLeft = left + column * (columnWidth + gap);
    const int panelTop = top + row * (rowHeight + gap);
    return RECT{panelLeft, panelTop, panelLeft + columnWidth, panelTop + rowHeight};
  };

  NativeDashboardLayout layout;
  const RECT airPanel = panel(0, 0);
  const RECT centerTop = panel(1, 0);
  layout.air = RECT{airPanel.left + 10, airPanel.top + 34, airPanel.right - 10,
                    airPanel.top + 104};
  if (layout.air.bottom <= layout.air.top) layout.air.bottom = layout.air.top + 1;
  if (layout.air.right <= layout.air.left) layout.air.right = layout.air.left + 1;
  layout.airHistory = NormalizeInsetRect(
      RECT{airPanel.left + 10, airPanel.top + 112, airPanel.right - 10, airPanel.bottom - 10},
      0, 0, 0, 0);
  layout.controls = panel(2, 1);
  layout.news = NormalizeInsetRect(
      RECT{centerTop.left + 12, centerTop.top + 8, centerTop.right - 12, centerTop.top + 92},
      0, 0, 0, 0);
  layout.weather = NormalizeInsetRect(
      RECT{centerTop.left + 12, centerTop.bottom - 108, centerTop.right - 12, centerTop.bottom - 8},
      0, 0, 0, 0);
  layout.energy = panel(2, 0);
  layout.stationhead = panel(0, 1);
  layout.radar = panel(1, 1);
  layout.clock = NormalizeInsetRect(
      RECT{centerTop.left + 12, centerTop.top + 34, centerTop.right - 12,
           centerTop.top + std::max(96L, centerTop.bottom - centerTop.top - 94)},
      0, 0, 0, 0);
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

  bool EnsureNativeClockWindow();
  void ApplyNativeClockBounds();
  void DestroyNativeClockWindow();
  bool EnsureNativeStaticWindows();
  void ApplyNativeStaticBounds();
  void DestroyNativeStaticWindows();
  void UpdateNativeStaticPanels(const RenderState& state);
  static LRESULT CALLBACK NativeClockWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  static LRESULT CALLBACK NativeStaticWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT HandleNativeClockMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT HandleNativeStaticMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  void PaintNativeClock(HWND hwnd);
  void PaintNativeAir(HWND hwnd);
  void PaintNativeAirHistory(HWND hwnd);
  void PaintNativeControls(HWND hwnd);
  void PaintNativeNews(HWND hwnd);
  void PaintNativeWeather(HWND hwnd);
  void PaintNativeEnergy(HWND hwnd);
  void PaintNativeStationhead(HWND hwnd);
  void PaintNativeRadar(HWND hwnd);
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
  RECT ClientBounds() const;
  void ParseDashboardMetadata(const std::wstring& json);

  HWND window_{};
  HWND nativeClockWindow_{};
  HWND nativeAirWindow_{};
  HWND nativeAirHistoryWindow_{};
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
  fs::path uiDir_;
  fs::path userDataDir_;
  std::atomic<bool> shuttingDown_{false};
  std::string dashboardUtf8_;
  std::wstring dashboardJson_ = L"{}";
  uint64_t dashboardSourceRevision_ = 0;
  std::string spotifyUtf8_;
  std::wstring spotifyJson_ = L"{}";
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
