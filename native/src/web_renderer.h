#pragma once
#include "common.h"
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

class Renderer {
 public:
  Renderer(HWND window, int width, int height, RadarManager& radar);
  ~Renderer();
  void Initialize();
  void Resize(int width, int height);
  void SetBounds(const RECT& bounds);
  void SetVisible(bool visible);
  bool IsUiReady() const noexcept { return ready_ && uiReady_; }
  HWND DashboardHostWindow() const noexcept { return dashboardHost_; }
  bool LoadDashboard(const fs::path& jsonPath, bool* changed = nullptr);
  int NewsCount() const { return newsCount_; }
  std::wstring MonitorHostHandle() const { return monitorHostHandle_; }
  void Render(const RECT& dirty, const RenderState& state);
  void UpdateState(const RenderState& state);
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
    std::wstring resolved;
    std::wstring error;
    int64_t fetchedAt = 0;
    uint64_t revision = 0;
    bool hasPayload = false;
  };

  struct StateJsonCache {
    std::wstring dashboard = L"{}";
    std::wstring spotify = L"{}";
    std::wstring airHistory = L"[]";
    std::wstring sensors = L"{}";
    std::wstring stationhead = L"{}";
    std::wstring toast;
    std::wstring appVersion;
    SensorSnapshot sensorsSource;
    StationheadStatus stationheadSource;
    uint64_t dashboardSourceRevision = 0;
    uint64_t spotifySourceRevision = 0;
    size_t airHistorySourceCount = 0;
    int64_t airHistorySourceLastT = 0;
    uint64_t dashboardRevision = 0;
    uint64_t spotifyRevision = 0;
    uint64_t airHistoryRevision = 0;
    uint64_t sensorsRevision = 0;
    uint64_t stationheadRevision = 0;
    uint64_t appVersionRevision = 0;
    uint64_t newsRevision = 0;
    int workspaceTab = 0;
    int newsIndex = 0;
    bool initialized = false;
  };

  void CreateWebView();
  void ConfigureWebView();
  void CloseWebView();
  bool EnsureDashboardHostWindow();
  void ApplyDashboardHostBounds();
  void DestroyDashboardHostWindow();
  bool EnsureNativeClockWindow();
  void ApplyNativeClockBounds();
  void DestroyNativeClockWindow();
  static LRESULT CALLBACK NativeClockWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT HandleNativeClockMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  void PaintNativeClock(HWND hwnd);
  void QueueAction(UiAction action, float seekFraction = 0.0f);
  std::wstring BuildStateJson(const RenderState& state, bool full = false);
  std::wstring BuildCachedStateJson(uint32_t changedSlices, bool full) const;
  std::wstring AirHistoryJson(const std::vector<AirHistorySample>& history) const;
  void PostState(const std::wstring& json);
  void PostFullState();
  void StartNativePlaybackBridge();
  void StopNativePlaybackBridge() noexcept;
  void NativePlaybackLoop();
  void FlushNativePlaybackMessages();
  void SetWebViewError(const std::wstring& stage, HRESULT result);
  void DrawStartupFallback(const RECT& dirty) const;
  void AppendWebViewDiagnostic(const std::wstring& message) const;
  RECT ClientBounds() const;
  void ParseDashboardMetadata(const std::wstring& json);

  HWND window_{};
  HWND dashboardHost_{};
  HWND nativeClockWindow_{};
  int width_ = 0;
  int height_ = 0;
  RECT bounds_{};
  RadarManager& radar_;
  fs::path rootDir_;
  fs::path dataDir_;
  fs::path uiDir_;
  fs::path userDataDir_;
  ComPtr<ICoreWebView2Environment> environment_;
  ComPtr<ICoreWebView2Controller> controller_;
  ComPtr<ICoreWebView2> webview_;
  EventRegistrationToken navigationStartingToken_{};
  EventRegistrationToken navigationToken_{};
  EventRegistrationToken messageToken_{};
  EventRegistrationToken processFailedToken_{};
  std::atomic<bool> creating_{false};
  std::atomic<bool> shuttingDown_{false};
  bool ready_ = false;
  bool uiReady_ = false;
  bool statePublishedForPendingPaint_ = false;
  bool controllerVisible_ = false;
  bool controllerBoundsValid_ = false;
  bool radarUpdatePending_ = false;
  RECT appliedControllerBounds_{};
  std::string dashboardUtf8_;
  std::wstring dashboardJson_ = L"{}";
  uint64_t dashboardSourceRevision_ = 0;
  std::string spotifyUtf8_;
  std::wstring spotifyJson_ = L"{}";
  uint64_t spotifySourceRevision_ = 0;
  std::wstring postedState_;
  StateJsonCache stateJsonCache_;
  std::wstring runtimeVersion_;
  std::wstring webViewError_;
  int64_t webViewStartedAt_ = 0;
  mutable bool diagnosticDirectoryEnsured_ = false;
  mutable std::wstring lastDiagnosticMessage_;
  mutable int64_t lastDiagnosticLoggedAt_ = 0;
  int newsCount_ = 0;
  std::wstring monitorHostHandle_;
  mutable std::mutex actionMutex_;
  mutable std::mutex diagnosticMutex_;
  UiAction pendingAction_ = UiAction::None;
  float pendingSeekFraction_ = 0.0f;
  std::thread nativePlaybackThread_;
  std::condition_variable nativePlaybackWake_;
  std::mutex nativePlaybackWakeMutex_;
  std::mutex nativePlaybackMutex_;
  std::array<NativePlaybackUpdate, 2> nativePlaybackUpdates_{};
  std::array<uint64_t, 2> nativePlaybackPostedRevisions_{};
  std::atomic<uint64_t> nativePlaybackRevision_{0};
  std::atomic<bool> nativePlaybackStarted_{false};
  std::atomic<bool> nativePlaybackStopping_{false};
};
}  // namespace hp
