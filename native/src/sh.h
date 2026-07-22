#pragma once
#include "common.h"
#include "config.h"
#include "logger.h"
#include "shared_immutable_vector.h"

namespace hp {
enum class StationheadTabKind {
  None,
  Stationhead,
  Auth,
};

// Which Stationhead window this player instance backs. The two roles share
// every behavior - script injection, resource blocking, the native click
// bridge, layout/visibility rules - except for exactly two things: the
// WebView2 user-data environment and which
// periodic poll runs (Primary polls Stationhead's authenticated stats API,
// Secondary runs a lightweight local-only auth probe).
enum class StationheadRole {
  Primary,
  Secondary,
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
  // App handles advance these when a primary/secondary notification or the
  // local track-transition projection changes. Keeping them first lets the
  // default equality operator reject changed snapshots before touching URLs
  // and diagnostic strings.
  uint64_t contentRevision = 0;
  uint64_t secondaryContentRevision = 0;
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
  bool secondaryPlaying = false;
  bool primaryAudioSelected = true;
  std::wstring url;
  // Render-only routing metadata for choosing the shared playback feed.
  std::wstring fallbackUrl;
  std::wstring secondaryUrl;
  std::wstring detail;
  // Recent per-day listening activity returned by the primary window's
  // authenticated Stationhead account endpoint, oldest first; the last entry
  // is today (partial, still accumulating). Empty for the secondary window.
  SharedImmutableVector<StationheadDailyPlayPoint> dailyPlayCounts;
  int64_t dailyPlayStatsUpdatedAt = 0;

  bool operator==(const StationheadStatus&) const = default;
};

// Drives one embedded Stationhead WebView2 window. Both Window A (Primary)
// and Window B (Secondary) are the same class, distinguished only by `role_`.
class StationheadPlayer {
 public:
  StationheadPlayer(StationheadRole role, HWND window, StationheadConfig config,
                     fs::path userDataFolder, Logger& log);
  StationheadPlayer(const StationheadPlayer&) = delete;
  StationheadPlayer& operator=(const StationheadPlayer&) = delete;
  ~StationheadPlayer();

  void Start();
  void Stop();
  void Tick(int64_t nowMs);
  [[nodiscard]] int64_t NextWakeAt() const noexcept { return nextTickAt_; }
  void RequestImmediateTick() noexcept { nextTickAt_ = 0; }
  [[nodiscard]] bool AudioPlaying() const noexcept {
    return audioPlaying_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] int64_t AudioPlayingSince() const noexcept {
    return audioPlayingSinceAt_.load(std::memory_order_relaxed);
  }
  void Reconnect();
  void RetryPendingTrackBoundaryRefresh(int64_t nowMs) {
    HandleTrackEnded(nowMs, true);
  }
  void CancelPendingTrackBoundaryRefresh() noexcept {
    trackBoundaryRefreshPending_ = false;
  }
  void SetPlaybackFallback(bool active, const std::wstring& reason);
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
  [[nodiscard]] bool SurfaceVisible() const noexcept {
    return startupPreviewActive_ || viewVisible_;
  }
  void KeepPlaybackBehindDashboard();

 private:
  [[nodiscard]] bool IsSecondary() const noexcept { return role_ == StationheadRole::Secondary; }
  // Tags shared log lines with which window they came from - both roles run
  // the same code path, so without this a log reader cannot tell whether an
  // "audio playing"/"audio stopped" entry (or any other shared-path log line)
  // came from Window A or Window B.
  [[nodiscard]] const wchar_t* RoleTag() const noexcept { return IsSecondary() ? L"B" : L"A"; }
  void ApplyMute() const noexcept;
  void ApplyVolume() const noexcept;
  void ApplyAudioPlaybackState(bool playing, const std::wstring& source);
  void HandleTrackEnded(int64_t nowMs, bool retry);
  void RecoverTrackBoundaryPlayback();
  void TryStartInitialNavigation();
  void CompletePendingAuthPopupDeferral() noexcept;
  void EnsureDistinctBrowserIdentity() noexcept;
  void Create();
  HRESULT CreateProfileController(
      HWND parentWindow, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* handler) const noexcept;
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
  void PollAuthProbe(int64_t nowMs);
  void AttemptNativeStartClick(int64_t nowMs);
  void FinishSpotifyAuthorization(const std::wstring& detail);
  void NavigateCurrentUrl(int64_t nowMs, const std::wstring& reason);
  std::wstring CurrentStationheadUrl() const;
  void NavigateStationheadUrl(int64_t nowMs, const std::wstring& url,
                              const std::wstring& reason, bool fallbackActive);
  bool NeedsInteractiveWindow() const;
  void SetStartupBounds();
  void SetVisible(bool visible);
  void ScheduleRecreate(const std::wstring& reason, int64_t delayMs = 0);
  void LayoutControllers();

  StationheadRole role_;
  HWND window_;
  HWND hostWindow_{};
  HWND authHostWindow_{};
  StationheadConfig config_;
  fs::path userDataFolder_;
  std::wstring profileName_;
  Logger& log_;
  mutable std::mutex mutex_;
  RECT bounds_{0, 0, 1, 1};
  StationheadTabKind selectedTab_ = StationheadTabKind::None;
  StationheadStatus status_;
  ComPtr<ICoreWebView2Environment> environment_;
  ComPtr<ICoreWebView2Controller> controller_;
  ComPtr<ICoreWebView2> webview_;
  ComPtr<ICoreWebView2Controller> authController_;
  ComPtr<ICoreWebView2> authWebview_;
  ComPtr<ICoreWebView2Deferral> authPopupDeferral_;
  std::shared_ptr<std::atomic<bool>> authPopupDeferralCompleted_;
  EventRegistrationToken navigationStartingToken_{};
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
  std::atomic<uint64_t> activeNavigationId_{0};
  std::atomic<bool> navigationInFlight_{false};
  bool trackBoundaryRefreshPending_ = false;
  bool trackBoundaryPlaybackRecoveryPending_ = false;
  bool trackBoundaryPlaybackRecoveryAwaitingNavigation_ = false;
  int64_t trackBoundaryPlaybackRecoveryDeadline_ = 0;
  int64_t creationStartedAt_ = 0;
  int64_t recreateAt_ = 0;
  std::atomic<bool> shuttingDown_{false};
  std::atomic<bool> audioPlaying_{false};
  std::atomic<int64_t> audioPlayingSinceAt_{0};
  std::atomic<bool> audioMuted_{false};
  std::atomic<double> audioVolume_{1.0};
  mutable std::atomic<int> appliedMuted_{-1};
  mutable std::atomic<int> appliedVolumePercent_{-1};
  std::atomic<uint32_t> pendingChangeFlags_{0};
  std::atomic<bool> changeMessagePending_{false};
  std::wstring pendingAuthorizationUrl_;
  int64_t createdAt_ = 0;
  int64_t startupScriptDeadline_ = 0;
  int64_t authControllerStartedAt_ = 0;
  int64_t lastReloadAt_ = 0;
  int64_t lastDailyPlayStatsAt_ = 0;  // Primary only.
  int64_t lastAuthProbeAt_ = 0;       // Secondary only.
  int64_t authProbeStartedAt_ = 0;    // Secondary only.
  bool authProbeInFlight_ = false;    // Secondary only.
  int64_t nextAutoClickAt_ = 0;
  bool autoClickInFlight_ = false;
  bool webViewConfigured_ = false;
  bool authCaptureScriptRegistrationComplete_ = false;
  bool startupScriptRegistrationComplete_ = false;
  bool startupNavigationStarted_ = false;
  bool stationNavigationStarted_ = false;
  int64_t nextTickAt_ = 0;
  std::wstring authPendingUrl_;
  bool spotifyAuthorization_ = false;
  bool loginRequired_ = false;
  bool nativeAudioTracking_ = false;
  bool viewVisible_ = false;
  bool startupPreviewActive_ = false;
  bool usingFallback_ = false;
  ICoreWebView2* identityWebview_ = nullptr;  // Secondary only.
};
}  // namespace hp
