#pragma once
#include "cloud_client.h"
#include "config.h"
#include "logger.h"
#include "radar.h"
#include "web_renderer.h"
#include "sensors.h"
#include "sh.h"
#include "secondary_sh.h"
#include "update_client.h"

namespace hp {
class App;

enum class WorkspaceTab {
  Main = 0,
  Stationhead = 1,
  Auth = 2,
};

// Shared boilerplate for the two Stationhead window handles: bounds/audio
// state plumbing and raising the playback surface above or below the
// dashboard. The primary and secondary players have very different startup
// and auth flows (kept in the derived classes below), but the "how do we
// apply bounds/mute/volume and decide whether the window should be on top"
// logic was duplicated near-verbatim between them. Derived classes supply
// IsInteractive(status) for the one rule that differs (which status fields
// count as "the user is looking at this window right now").
template <typename Derived, typename PlayerT>
class StationheadHandleBase {
 public:
  explicit operator bool() const noexcept { return static_cast<bool>(player_); }
  Derived* operator->() noexcept { return static_cast<Derived*>(this); }
  const Derived* operator->() const { return static_cast<const Derived*>(this); }

  void Stop() { if (player_) player_->Stop(); }

  void SetAudioMuted(bool muted) noexcept {
    audioMuted_ = muted;
    ApplyAudioState();
  }
  void ToggleAudioMuted() noexcept { SetAudioMuted(!audioMuted_); }
  bool AudioMuted() const noexcept { return audioMuted_; }
  void SetAudioVolume(double volume) noexcept {
    audioVolume_ = std::clamp(volume, 0.0, 1.0);
    ApplyAudioState();
  }
  double AudioVolume() const noexcept { return audioVolume_; }

  void SetBounds(const RECT& bounds) {
    workspaceBounds_ = bounds;
    ApplyBounds();
  }

 protected:
  void ApplyAudioState() const noexcept {
    if (!player_) return;
    player_->SetMuted(audioMuted_);
    player_->SetVolume(audioVolume_);
  }

  void BringMainWindowToFront(HWND host) const noexcept {
    if (!host || !IsWindow(host)) return;
    HWND root = GetAncestor(host, GA_ROOT);
    if (!root || !IsWindow(root)) return;
    SetWindowPos(root, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    UpdateWindow(root);
  }

  void RaiseActiveHost() const {
    if (!player_) return;
    HWND host = player_->ActiveHostWindowForAccountSetup();
    if (!host || !IsWindow(host)) return;
    const auto status = player_->Status();
    const bool interactive = static_cast<const Derived*>(this)->IsInteractive(status);
    const bool compactPlayback = status.lightweight && !interactive;
    const int width = compactPlayback
        ? 2
        : std::max(1L, workspaceBounds_.right - workspaceBounds_.left);
    const int height = compactPlayback
        ? 2
        : std::max(1L, workspaceBounds_.bottom - workspaceBounds_.top);
    const HWND placement = interactive ? HWND_TOP : HWND_BOTTOM;
    SetWindowPos(host, placement, workspaceBounds_.left, workspaceBounds_.top,
                 width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    BringMainWindowToFront(host);
  }

  void ApplyInteractiveBounds() {
    if (player_) player_->SetBounds(workspaceBounds_);
  }

  void ApplyBounds() {
    if (!player_) return;
    ApplyAudioState();
    player_->SetBounds(workspaceBounds_);
    RaiseActiveHost();
  }

  std::unique_ptr<PlayerT> player_;
  RECT workspaceBounds_{0, 0, 1, 1};
  bool audioMuted_ = false;
  double audioVolume_ = 1.0;
};

// App-facing owner for the primary player. Its playback surface remains behind
// the dashboard unless the user explicitly opens the Stationhead or auth tab.
class AppStationheadHandle : public StationheadHandleBase<AppStationheadHandle, StationheadPlayer> {
 public:
  AppStationheadHandle() = default;
  AppStationheadHandle(const AppStationheadHandle&) = delete;
  AppStationheadHandle& operator=(const AppStationheadHandle&) = delete;

  AppStationheadHandle& operator=(std::unique_ptr<StationheadPlayer> player) noexcept {
    player_ = std::move(player);
    ApplyAudioState();
    ApplyBounds();
    return *this;
  }

  void reset() noexcept {
    player_.reset();
    selectedTab_ = StationheadTabKind::None;
  }

  void Start() {
    if (!player_) return;
    ApplyInteractiveBounds();
    player_->Start();
    ApplyAudioState();
    ApplyBounds();
  }
  void Tick(int64_t nowMs, bool diagnosticsVisible = false) {
    if (!player_) return;
    player_->Tick(nowMs, diagnosticsVisible);
    ApplyAudioState();
    ApplyBounds();
  }
  void Reconnect() {
    if (!player_) return;
    ApplyInteractiveBounds();
    player_->Reconnect();
    ApplyAudioState();
    ApplyBounds();
  }
  void RefreshSpotifyState(bool notify = true) {
    if (player_) player_->RefreshSpotifyState(notify);
  }
  void ShowForLogin() {
    if (!player_) return;
    ApplyInteractiveBounds();
    player_->ShowForLogin();
    ApplyAudioState();
    ApplyBounds();
  }
  void ShowAfterAudioStop() {
    if (!player_) return;
    ApplyInteractiveBounds();
    player_->ShowAfterAudioStop();
    ApplyAudioState();
    ApplyBounds();
  }
  void OpenSpotifyAuthorization(const std::wstring& url) {
    if (!player_) return;
    ApplyInteractiveBounds();
    player_->OpenSpotifyAuthorization(url);
    ApplyAudioState();
    ApplyBounds();
  }
  void ReleaseCompletedAuth() {
    if (!player_) return;
    player_->ReleaseCompletedAuth();
    ApplyAudioState();
    ApplyBounds();
  }
  void ToggleView() {
    if (!player_) return;
    ApplyInteractiveBounds();
    player_->ToggleView();
    ApplyAudioState();
    ApplyBounds();
  }
  uint32_t ConsumeChangeFlags() {
    return player_ ? player_->ConsumeChangeFlags() : StationheadChangeNone;
  }
  bool HasAuthTab() const { return player_ && player_->HasAuthTab(); }
  StationheadStatus Status() const;
  void NotifyMonitorHandle(const std::wstring& handle) {
    if (!player_) return;
    player_->NotifyMonitorHandle(handle);
    ApplyAudioState();
    ApplyBounds();
  }

  void SelectTab(StationheadTabKind tab) {
    selectedTab_ = tab;
    if (!player_) return;
    if (tab != StationheadTabKind::None) ApplyInteractiveBounds();
    player_->SelectTab(tab);
    ApplyAudioState();
    ApplyBounds();
  }

  // selectedTab_ here only reflects tab changes made through this wrapper's
  // own SelectTab(); internal transitions such as ShowForLogin() change the
  // player's visibility without going through it, leaving selectedTab_ stale
  // at None and sending the host to the bottom even while a login prompt or
  // auth flow is actively shown. Use the player's live status instead.
  bool IsInteractive(const StationheadStatus& status) const noexcept {
    return status.visible || status.loginRequired || status.authAvailable;
  }

 private:
  StationheadTabKind selectedTab_ = StationheadTabKind::None;
};

// The secondary player retains its isolated WebView2 profile. Its audio output
// is user-toggleable and its surface is temporarily raised for account setup.
class AppSecondaryStationheadHandle
    : public StationheadHandleBase<AppSecondaryStationheadHandle, SecondaryStationheadPlayer> {
 public:
  AppSecondaryStationheadHandle() = default;
  AppSecondaryStationheadHandle(const AppSecondaryStationheadHandle&) = delete;
  AppSecondaryStationheadHandle& operator=(const AppSecondaryStationheadHandle&) = delete;

  AppSecondaryStationheadHandle& operator=(
      std::unique_ptr<SecondaryStationheadPlayer> player) noexcept {
    player_ = std::move(player);
    stationheadAuthorizationSeen_ = false;
    apiAuthorizationActive_ = false;
    ApplyAudioState();
    ApplyBounds();
    return *this;
  }

  void reset() noexcept {
    player_.reset();
    stationheadAuthorizationSeen_ = false;
    apiAuthorizationActive_ = false;
  }

  void Start() {
    if (!player_) return;
    ApplyInteractiveBounds();
    player_->Start();
    ApplyAudioState();
    ApplyBounds();
  }
  void Tick(int64_t nowMs) {
    if (!player_) return;
    player_->Tick(nowMs);
    ApplyAudioState();

    const SecondaryStationheadStatus status = player_->Status();
    if (status.spotifyAuthorization && !status.apiAuthorization) {
      stationheadAuthorizationSeen_ = true;
    }
    if (status.apiAuthorization) {
      apiAuthorizationActive_ = true;
    } else if (apiAuthorizationActive_) {
      apiAuthorizationActive_ = false;
      stationheadAuthorizationSeen_ = false;
      if (status.detail.find(L"Spotify API authentication completed") !=
          std::wstring::npos) {
        if (CloudClient* cloud = CloudClient::Current()) cloud->RefreshNow();
      }
    } else if (stationheadAuthorizationSeen_ && !status.spotifyAuthorization) {
      const bool stationheadConnected =
          status.detail.find(L"Spotify authentication completed") !=
          std::wstring::npos;
      stationheadAuthorizationSeen_ = false;
      if (stationheadConnected) {
        if (CloudClient* cloud = CloudClient::Current()) {
          const std::wstring authorizationUrl = cloud->BeginSpotifyAuthorization();
          if (!authorizationUrl.empty() &&
              player_->OpenSpotifyApiAuthorization(authorizationUrl)) {
            apiAuthorizationActive_ = true;
          }
        }
      }
    }

    ApplyBounds();
  }
  void Reconnect() {
    if (!player_) return;
    stationheadAuthorizationSeen_ = false;
    apiAuthorizationActive_ = false;
    ApplyInteractiveBounds();
    player_->Reconnect();
    ApplyAudioState();
    ApplyBounds();
  }
  SecondaryStationheadStatus Status() const {
    ApplyAudioState();
    SecondaryStationheadStatus status = player_ ? player_->Status() : SecondaryStationheadStatus{};
    status.audioMuted = audioMuted_;
    return status;
  }

  bool IsInteractive(const SecondaryStationheadStatus& status) const noexcept {
    return status.visible || status.loginRequired ||
        status.spotifyAuthorization || status.apiAuthorization;
  }

 private:
  bool stationheadAuthorizationSeen_ = false;
  bool apiAuthorizationActive_ = false;
};

class App {
 public:
  explicit App(HINSTANCE instance);
  ~App();
  int Run(int showCommand);
  static App* Current();
  void LogUnhandled(DWORD code, void* address);
  void ToggleStationheadAudioA() {
    stationhead_->ToggleAudioMuted();
    renderState_.toast = stationhead_->AudioMuted()
        ? L"Stationhead A 音声OFF"
        : L"Stationhead A 音声ON";
    toastUntil_ = UnixMillis() + 3000;
    InvalidateAll();
  }
  void ToggleStationheadAudioB() {
    if (secondaryStationhead_) {
      secondaryStationhead_->ToggleAudioMuted();
      renderState_.toast = secondaryStationhead_->AudioMuted()
          ? L"Stationhead B 音声OFF"
          : L"Stationhead B 音声ON";
    } else {
      renderState_.toast = L"Stationhead B は未設定です";
    }
    toastUntil_ = UnixMillis() + 3000;
    InvalidateAll();
  }
  void SetStationheadVolumeA(double volume) noexcept {
    stationhead_->SetAudioVolume(volume);
  }
  void SetStationheadVolumeB(double volume) noexcept {
    if (secondaryStationhead_) secondaryStationhead_->SetAudioVolume(volume);
  }

  // UI-ready and primary-audio probes are advisory startup signals. A stalled
  // probe must not permanently suppress the independently profiled secondary
  // player. Prefer the normal ready path, then use a bounded fallback.
  bool NeedsSecondaryStartupFallback() const noexcept {
    if (secondaryStarted_ || !secondaryStationhead_ || startupAt_ <= 0) return false;
    const int64_t elapsed = UnixMillis() - startupAt_;
    const bool dashboardReady = renderer_ && renderer_->IsUiReady();
    return (dashboardReady && elapsed >= 45'000) || elapsed >= 90'000;
  }

 private:
  class RendererStartupState {
   public:
    RendererStartupState& operator=(bool started) noexcept {
      started_ = started;
      return *this;
    }

    // Initialization is guarded by the raw started flag. Secondary startup
    // normally waits for the HTML ready message, but a broken/missed WebView2
    // signal receives an explicit bounded fallback instead of waiting forever.
    bool operator!() const noexcept { return !started_; }
    explicit operator bool() const noexcept {
      const App* app = App::Current();
      if (!started_ || !app || !app->renderer_) return false;
      if (app->renderer_->IsUiReady()) return true;
      const bool fallback = app->NeedsSecondaryStartupFallback();
      if (fallback && !fallbackLogged_ && app->logger_) {
        fallbackLogged_ = true;
        app->logger_->Warn(
            L"Dashboard ready signal timed out; allowing secondary Stationhead startup fallback");
      }
      return fallback;
    }

   private:
    bool started_ = false;
    mutable bool fallbackLogged_ = false;
  };

  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
  LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
  void InitializePaths();
  void CreateMainWindow(int showCommand);
  void StartServices();
  void StartDeferredServices(int64_t now, const StationheadStatus& stationheadStatus);
  void StopServices();
  void Tick();
  void Draw();
  void PublishRenderState();
  void InvalidateAll();
  void Invalidate(const RECT& rect);
  void LoadAirHistory();
  void SaveAirHistory() const;
  void UpdateAirHistory(const SensorSnapshot& sensors);
  void HandleAction(UiAction action, float seekFraction);
  void LayoutWorkspace();
  void ProcessRemoteCommands();
  void SendTelemetryAsync();
  void RefreshDiagnostics();
  void ClearDisplayCache();
  void CheckForUpdateAsync(bool install);
  bool LaunchVerifiedUpdater(const std::wstring& version, const std::string& manifestJson);
  static std::wstring FormatTimestamp(int64_t timestamp);
  static size_t ProcessWorkingSet(DWORD pid);
  static double ProcessCpuPercent();

  HINSTANCE instance_{};
  HWND window_{};
  HANDLE mutex_{};
  fs::path rootDir_;
  fs::path dataDir_;
  AppConfig config_;
  std::unique_ptr<Logger> logger_;
  std::unique_ptr<RadarManager> radar_;
  std::unique_ptr<Renderer> renderer_;
  std::unique_ptr<CloudClient> cloud_;
  std::unique_ptr<SensorHub> sensors_;
  AppStationheadHandle stationhead_;
  AppSecondaryStationheadHandle secondaryStationhead_;
  RenderState renderState_;
  std::atomic<bool> telemetryBusy_{false};
  std::atomic<bool> updateBusy_{false};
  std::thread telemetryThread_;
  std::thread updateThread_;
  bool running_ = false;
  int exitCode_ = 0;
  int64_t startupAt_ = 0;
  int64_t playbackReadyAt_ = 0;
  int64_t secondaryEligibleAt_ = 0;
  bool secondaryStarted_ = false;
  RendererStartupState rendererStarted_;
  bool cloudStarted_ = false;
  bool startupUpdateScheduled_ = false;
  int64_t lastTelemetryAt_ = 0;
  int64_t lastDiagnosticAt_ = 0;
  int64_t lastPerformanceLogAt_ = 0;
  uint64_t lastRadarFrameStamp_ = 0;
  int64_t toastUntil_ = 0;
  // News rotation managed by native timer; index sent to WebView in state JSON
  int newsIndex_ = 0;
  int newsCount_ = 0;
  int64_t lastNewsRotateAt_ = 0;
  WorkspaceTab selectedTab_ = WorkspaceTab::Main;
  RECT workspaceBounds_{0, 0, 1, 1};
  inline static App* current_ = nullptr;
};

inline StationheadStatus AppStationheadHandle::Status() const {
  StationheadStatus status = player_ ? player_->Status() : StationheadStatus{};
  status.audioMuted = audioMuted_;
  if (!status.audioPlaying && !status.lightweight) {
    const App* app = App::Current();
    if (app && app->NeedsSecondaryStartupFallback()) {
      // This flag is consumed only as App's startup-readiness signal. The
      // underlying StationheadPlayer state remains untouched, so playback,
      // telemetry, and controller layout continue to use the real status.
      status.lightweight = true;
    }
  }
  return status;
}
}  // namespace hp
