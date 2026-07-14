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
  StationheadChangeShowPlayer = 1u << 2,
};

struct StationheadDailyPlayPoint {
  int64_t dayStartMsUtc = 0;
  int value = 0;

  bool operator==(const StationheadDailyPlayPoint&) const = default;
};

// A single 5-minute sample of today's cumulative play value, kept over time
// so a flattening of consecutive values can be read back later as a gap in
// listening activity (see App::UpdateStationheadPlayHistory).
struct StationheadPlayHistorySample {
  int64_t timestamp = 0;
  int value = 0;

  bool operator==(const StationheadPlayHistorySample&) const = default;
};

struct StationheadStatus {
  bool created = false;
  bool navigating = false;
  bool playing = false;
  bool loginRequired = false;
  bool spotifyAuthorization = false;
  bool visible = false;
  bool processFailed = false;
  bool spotifyConfigured = false;
  bool authAvailable = false;
  bool audioPlaying = false;
  bool audioMuted = false;
  bool secondaryAudioMuted = false;
  std::wstring url;
  std::wstring detail;
  // Recent per-day listening activity for the account logged into the
  // primary Stationhead window, oldest first; the last entry is today
  // (partial, still accumulating). Sourced from the account's own
  // streakStats endpoint, so it reflects the site's own "val" unit
  // (observed to track closer to listening minutes than track plays).
  std::vector<StationheadDailyPlayPoint> dailyPlayCounts;
  int64_t dailyPlayStatsUpdatedAt = 0;

  bool operator==(const StationheadStatus&) const = default;
};

class StationheadPlayer {
 public:
  StationheadPlayer(HWND window, StationheadConfig config, fs::path userDataFolder, Logger& log);
  ~StationheadPlayer();
  void Start();
  void Stop();
  void Tick(int64_t nowMs);
  [[nodiscard]] int64_t NextWakeAt() const noexcept { return nextTickAt_; }
  void Reconnect();
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
  void SetStartupPreviewBounds(const RECT& bounds);
  void ClearStartupPreviewBounds();
  void SelectTab(StationheadTabKind tab);
  bool HasAuthTab() const;
  StationheadStatus Status() const;
  HWND ActiveHostWindowForAccountSetup() const noexcept;
  void KeepPlaybackBehindDashboard();

 private:
  void ApplyMute() const noexcept;
  void ApplyVolume() const noexcept;
  void ApplyAudioPlaybackState(bool playing, int64_t nowMs, const std::wstring& source);
  void Create();
  void EnsureAuthController(const std::wstring& url);
  bool EnsureHostWindow();
  bool EnsureAuthHostWindow();
  void CloseWebView();
  void CloseAuthWebView();
  void PostChange(uint32_t flags = StationheadChangeNone);
  void ConfigureWebView();
  void ConfigureAuthWebView();
  void ResetNavigationRouteState();
  void PollDailyPlayStats(int64_t nowMs);
  void NavigatePrimaryUrl(int64_t nowMs, const std::wstring& reason);
  void NavigateStationheadUrl(int64_t nowMs, const std::wstring& url,
                              const std::wstring& reason, bool fallbackActive);
  bool NeedsInteractiveWindow() const;
  void SetStartupBounds();
  void SetVisible(bool visible);
  void ScheduleRecreate(const std::wstring& reason);
  void LayoutControllers();

  HWND window_;
  HWND hostWindow_{};
  HWND authHostWindow_{};
  StationheadConfig config_;
  fs::path userDataFolder_;
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
  EventRegistrationToken audioPlayingChangedToken_{};
  std::atomic<bool> resourceBlockingArmed_{false};
  EventRegistrationToken authNavigationToken_{};
  EventRegistrationToken authMessageToken_{};
  EventRegistrationToken authProcessFailedToken_{};
  EventRegistrationToken authCloseToken_{};
  std::shared_ptr<std::atomic<bool>> createCallbackAlive_{
      std::make_shared<std::atomic<bool>>(false)};
  std::shared_ptr<std::atomic<bool>> authCallbackAlive_{
      std::make_shared<std::atomic<bool>>(false)};
  std::atomic<bool> creating_{false};
  std::atomic<bool> recreating_{false};
  std::atomic<bool> shuttingDown_{false};
  std::atomic<bool> audioPlaying_{false};
  std::atomic<bool> audioMuted_{false};
  std::atomic<double> audioVolume_{1.0};



  mutable std::atomic<int> appliedMuted_{-1};
  mutable std::atomic<int> appliedVolumePercent_{-1};
  std::atomic<uint32_t> pendingChangeFlags_{0};
  std::atomic<bool> changeMessagePending_{false};
  std::wstring pendingAuthorizationUrl_;
  int64_t createdAt_ = 0;
  int64_t lastReloadAt_ = 0;
  int64_t lastDailyPlayStatsAt_ = 0;
  int64_t noAudioSinceAt_ = 0;
  int64_t fallbackMonitorAfterAt_ = 0;
  int64_t nextTickAt_ = 0;
  std::wstring authPendingUrl_;
  bool spotifyAuthorization_ = false;
  bool loginSessionActive_ = false;
  bool nativeAudioTracking_ = false;
  bool viewVisible_ = false;
  bool startupPreviewActive_ = false;
  bool usedFallback_ = false;
};
}
