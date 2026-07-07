#pragma once
#include "common.h"
#include "config.h"
#include "logger.h"

namespace hp {
enum class StationheadTabKind {
  None,
  Stationhead,
  Auth,
};

enum StationheadChangeFlags : uint32_t {
  StationheadChangeNone = 0,
  StationheadChangeReturnMain = 1u << 0,
  StationheadChangeReleaseAuth = 1u << 1,
  StationheadChangeSpotifyState = 1u << 2,
  StationheadChangeShowPlayer = 1u << 3,
};

struct StationheadStatus {
  bool created = false;
  bool navigating = false;
  bool playing = false;
  bool loginRequired = false;
  bool lightweight = false;
  bool visible = false;
  bool processFailed = false;
  bool spotifyConfigured = false;
  bool authAvailable = false;
  bool audioPlaying = false;
  bool audioSilent = false;
  bool audioMuted = false;
  bool secondaryAudioMuted = false;
  int healthMisses = 0;
  int64_t lastPlaybackConfirmedAt = 0;
  size_t processWorkingSet = 0;
  double processCpuPercent = 0;
  uint64_t blockedResources = 0;
  std::wstring url;
  std::wstring detail;
  std::wstring trackTitle;
  std::wstring trackArtist;
  std::wstring deviceName;
  std::wstring artworkUrl;
  int64_t sampledAt = 0;
  int64_t expectedEndAt = 0;
  int64_t trackDurationMs = 0;
};

class StationheadPlayer {
 public:
  StationheadPlayer(HWND window, StationheadConfig config, fs::path userDataFolder, Logger& log);
  ~StationheadPlayer();
  void Start();
  void Stop();
  void Tick(int64_t nowMs, bool diagnosticsVisible = false);
  void Reconnect();
  void RefreshSpotifyState(bool notify = true);
  void RefreshLocalMetadata(int64_t) noexcept {}
  void ShowForLogin();
  void ShowAfterAudioStop();
  void OpenSpotifyAuthorization(const std::wstring& url);
  void ReleaseCompletedAuth();
  void ToggleView();
  uint32_t ConsumeChangeFlags();
  void SetMuted(bool muted) noexcept;
  bool Muted() const noexcept;
  void SetVolume(double volume) noexcept;
  double Volume() const noexcept;
  void SetBounds(const RECT& bounds);
  void SelectTab(StationheadTabKind tab);
  bool HasAuthTab() const;
  StationheadStatus Status() const;
  HWND ActiveHostWindowForAccountSetup() const noexcept;
  void NotifyMonitorHandle(const std::wstring& handle);

 private:
  void ApplyMute() const noexcept;
  void ApplyVolume() const noexcept;
  void Create();
  void EnsureAuthController(const std::wstring& url);
  bool EnsureHostWindow();
  bool EnsureAuthHostWindow();
  void LayoutHostWindow(bool background);
  void CloseWebView();
  void CloseAuthWebView();
  void StartSpotifyStateWatcher();
  void StopSpotifyStateWatcher();
  void PostChange(uint32_t flags = StationheadChangeNone);
  void ConfigureWebView();
  void ConfigureAuthWebView();
  void EvaluateStartupState();
  void HandleStartupStateResult(HRESULT error, LPCWSTR result);
  void ClickTarget(double x, double y);
  void ResetNavigationRouteState(int64_t nowMs);
  void NavigatePrimaryUrl(int64_t nowMs, const std::wstring& reason);
  bool NeedsInteractiveWindow() const;
  void KeepPlaybackBehindDashboard();
  void SetVisible(bool visible);
  void ScheduleRecreate(const std::wstring& reason);
  size_t MeasureProcessWorkingSet();
  void LayoutControllers();

  HWND window_;
  HWND hostWindow_{};
  HWND authHostWindow_{};
  StationheadConfig config_;
  fs::path userDataFolder_;
  fs::path spotifyStatePath_;
  Logger& log_;
  mutable std::mutex mutex_;
  RECT bounds_{};
  StationheadTabKind selectedTab_ = StationheadTabKind::None;
  StationheadStatus status_;
  ComPtr<ICoreWebView2Environment> environment_;
  ComPtr<ICoreWebView2Controller> controller_;
  ComPtr<ICoreWebView2> webview_;
  ComPtr<ICoreWebView2Controller> authController_;
  ComPtr<ICoreWebView2> authWebview_;
  EventRegistrationToken navigationToken_{};
  EventRegistrationToken newWindowToken_{};
  EventRegistrationToken webMessageToken_{};
  EventRegistrationToken processFailedToken_{};
  EventRegistrationToken resourceRequestedToken_{};
  EventRegistrationToken authNavigationToken_{};
  EventRegistrationToken authMessageToken_{};
  EventRegistrationToken authProcessFailedToken_{};
  EventRegistrationToken authCloseToken_{};
  std::atomic<bool> creating_{false};
  std::atomic<bool> recreating_{false};
  std::atomic<bool> scanPending_{false};
  std::atomic<bool> shuttingDown_{false};
  std::atomic<bool> spotifyStateDirty_{true};
  std::atomic<bool> audioPlaying_{false};
  std::atomic<bool> audioStateKnown_{false};
  std::atomic<bool> audioMuted_{false};
  std::atomic<double> audioVolume_{1.0};
  std::atomic<uint32_t> pendingChangeFlags_{0};
  std::atomic<bool> changeMessagePending_{false};
  std::thread spotifyWatchThread_;
  HANDLE spotifyWatchStopEvent_{};
  std::wstring targetSignature_;
  std::wstring pendingAuthorizationUrl_;
  int stableTargetCount_ = 0;
  int64_t createdAt_ = 0;
  int64_t lastSpotifyCheckAt_ = 0;
  int64_t lastMemoryCheckAt_ = 0;
  int64_t lastReloadAt_ = 0;
  int64_t lastScanAt_ = 0;
  int64_t startupScanUntil_ = 0;
  int64_t lastAudioAtMs_ = 0;
  int64_t nextTickAt_ = 0;
  int64_t lastProcessSampleAt_ = 0;
  std::optional<fs::file_time_type> spotifyStateWriteTime_;
  std::wstring authPendingUrl_;
  bool spotifyAuthorization_ = false;
  bool loginSessionActive_ = false;
  bool showAfterNavigation_ = false;
  bool viewVisible_ = false;
  bool backgroundHostPlaced_ = false;
  bool controllerLayoutValid_ = false;
  bool lastLayoutHadAuthController_ = false;
  RECT lastControllerLayoutBounds_{};
  StationheadTabKind lastControllerLayoutTab_ = StationheadTabKind::None;
  bool usedFallback_ = false;
  bool usedSakurazaka_ = false;
  int64_t createdForAudioCheckAt_ = 0;
  bool waitingForStartTransition_ = false;
};
}  // namespace hp
