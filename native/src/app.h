#pragma once
#include "cloud_client.h"
#include "config.h"
#include "logger.h"
#include "web_renderer.h"
#include "sensors.h"
#include "sh.h"
#include "secondary_sh.h"
#include "update_client.h"

namespace hp {
class App;

template <typename StatusT>
inline bool StationheadNeedsForeground(const StatusT& status) noexcept {
  // Foreground is driven purely by a login prompt (status.loginRequired), which
  // the page scan sets when it sees a "log in" / "sign in" style term. The
  // Spotify/API authorization flags are intentionally NOT used: a window can be
  // playing audio (Spotify authorized) yet still need a Stationhead login, and a
  // stale auth flag must never pop a playing window in front of the dashboard.
  return status.loginRequired;
}

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

  void SetStartupPreviewBounds(const RECT& bounds) {
    startupPreviewBounds_ = bounds;
    startupPreviewActive_ = true;
    ApplyBounds();
  }

  void ClearStartupPreviewBounds() {
    if (!startupPreviewActive_) return;
    startupPreviewActive_ = false;
    ApplyBounds();
  }

  bool StartupPreviewActive() const noexcept { return startupPreviewActive_; }

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
    const bool preview = startupPreviewActive_;
    const RECT activeBounds = preview ? startupPreviewBounds_ : workspaceBounds_;
    const int width = std::max(1L, activeBounds.right - activeBounds.left);
    const int height = std::max(1L, activeBounds.bottom - activeBounds.top);
    const HWND placement = (interactive || preview) ? HWND_TOP : HWND_BOTTOM;
    SetWindowPos(host, placement, activeBounds.left, activeBounds.top,
                 width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (!preview) BringMainWindowToFront(host);
  }

  void ApplyInteractiveBounds() {
    if (!player_) return;
    player_->ClearStartupPreviewBounds();
    player_->SetBounds(workspaceBounds_);
  }

  void ApplyBounds() {
    if (!player_) return;
    ApplyAudioState();
    if (startupPreviewActive_) {
      player_->SetStartupPreviewBounds(startupPreviewBounds_);
    } else {
      player_->ClearStartupPreviewBounds();
      player_->SetBounds(workspaceBounds_);
    }
    RaiseActiveHost();
  }

  std::unique_ptr<PlayerT> player_;
  RECT workspaceBounds_{0, 0, 1, 1};
  RECT startupPreviewBounds_{0, 0, 1, 1};
  bool startupPreviewActive_ = false;
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
  // The periodic tick only converges audio state (cheap, deduplicated in the
  // player). Bounds and z-order are re-asserted on explicit events only:
  // startup/layout changes via SetBounds/SelectTab, login prompts via
  // ShowForLogin, and each playback confirmation after a (re)load, where the
  // player itself drops its window behind the dashboard again.
  void Tick(int64_t nowMs) {
    if (!player_) return;
    player_->Tick(nowMs);
    ApplyAudioState();
  }
  void Reconnect() {
    if (!player_) return;
    ApplyInteractiveBounds();
    player_->Reconnect();
    ApplyAudioState();
    ApplyBounds();
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
  StationheadStatus Status() const {
    StationheadStatus status = player_ ? player_->Status() : StationheadStatus{};
    status.audioMuted = audioMuted_;
    return status;
  }
  int64_t NextWakeAt() const noexcept {
    return player_ ? player_->NextWakeAt() : 0;
  }

  void SelectTab(StationheadTabKind tab) {
    selectedTab_ = tab;
    if (!player_) return;
    if (tab != StationheadTabKind::None) ApplyInteractiveBounds();
    player_->SelectTab(tab);
    ApplyAudioState();
    ApplyBounds();
  }

  // The primary Stationhead surface stays behind the dashboard except when a
  // login prompt is on the page. Playback state is irrelevant: a window can be
  // playing (Spotify authorized) yet still need a Stationhead login, so it must
  // come forward for the user to sign in.
  bool IsInteractive(const StationheadStatus& status) const noexcept {
    return StationheadNeedsForeground(status);
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
    ApplyAudioState();
    ApplyBounds();
    return *this;
  }

  void reset() noexcept {
    player_.reset();
  }

  void Start() {
    if (!player_) return;
    ApplyInteractiveBounds();
    player_->Start();
    ApplyAudioState();
    ApplyBounds();
  }
  // See AppStationheadHandle::Tick: periodic ticks only converge audio state.
  void Tick(int64_t nowMs) {
    if (!player_) return;
    player_->Tick(nowMs);
    ApplyAudioState();
  }
  void Reconnect() {
    if (!player_) return;
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
  int64_t NextWakeAt() const noexcept {
    return player_ ? player_->NextWakeAt() : 0;
  }

  bool IsInteractive(const SecondaryStationheadStatus& status) const noexcept {
    return StationheadNeedsForeground(status);
  }
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
    // Reflect the manual change in the render state immediately so the button
    // label updates on the very next paint instead of lagging until the next
    // Tick rebuild (idle ticks can be up to 30s apart). The 50-minute auto
    // rotation still re-asserts its A/B pattern on schedule.
    ApplyScheduledStationheadAudioProfile(!stationhead_->AudioMuted());
    renderState_.toast = stationhead_->AudioMuted()
        ? L"Stationhead A 音声OFF"
        : L"Stationhead A 音声ON";
    toastUntil_ = UnixMillis() + 3000;
    MarkRenderStateDirty();
    InvalidateAll();
  }
  void ToggleStationheadAudioB() {
    if (secondaryStationhead_) {
      secondaryStationhead_->ToggleAudioMuted();
      ApplyScheduledStationheadAudioProfile(secondaryStationhead_->AudioMuted());
      renderState_.toast = secondaryStationhead_->AudioMuted()
          ? L"Stationhead B 音声OFF"
          : L"Stationhead B 音声ON";
    } else {
      renderState_.toast = L"Stationhead B は未設定です";
    }
    toastUntil_ = UnixMillis() + 3000;
    MarkRenderStateDirty();
    InvalidateAll();
  }
  void SetStationheadVolumeA(double volume) noexcept {
    stationhead_->SetAudioVolume(volume);
  }
  void SetStationheadVolumeB(double volume) noexcept {
    if (secondaryStationhead_) secondaryStationhead_->SetAudioVolume(volume);
  }

 private:
  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
  LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
  void InitializePaths();
  void CreateMainWindow(int showCommand);
  void StartServices();
  void StartDeferredServices(int64_t now, const StationheadStatus& stationheadStatus);
  void ApplyStartupStationheadPreview();
  void ClearStartupStationheadPreview();
  void StopServices();
  void Tick();
  void Draw();
  void MarkRenderStateDirty() noexcept { renderStateDirty_ = true; }
  void ShowToast(std::wstring message, int64_t durationMs, bool invalidate = true);
  bool UpdateRenderStationheadState(const StationheadStatus& nextState);
  void ScheduleNextTick(uint32_t milliseconds);
  void ApplyScheduledStationheadAudioProfile(bool primaryAudible) noexcept;
  void PublishRenderState();
  void PublishRenderStateNow();
  void InvalidateAll();
  void LoadAirHistory();
  void SaveAirHistory() const;
  void UpdateAirHistory(const SensorSnapshot& sensors);
  void HandleAction(UiAction action);
  void LayoutWorkspace();
  void ProcessRemoteCommands();
  void SendTelemetryAsync();
  void ClearDisplayCache();
  void CheckForUpdateAsync(bool install);
  bool LaunchVerifiedUpdater(const std::wstring& version, const std::string& manifestJson);

  HINSTANCE instance_{};
  HWND window_{};
  HANDLE mutex_{};
  fs::path rootDir_;
  fs::path dataDir_;
  AppConfig config_;
  std::unique_ptr<Logger> logger_;
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
  bool secondaryStarted_ = false;
  bool rendererStarted_ = false;
  bool cloudStarted_ = false;
  bool startupUpdateScheduled_ = false;
  int64_t lastTelemetryAt_ = 0;
  uint64_t lastRadarFrameStamp_ = 0;
  int64_t toastUntil_ = 0;
  int64_t nextAppTickAt_ = 0;
  // News rotation managed by native timer; index sent to WebView in state JSON
  int newsIndex_ = 0;
  int newsCount_ = 0;
  int64_t lastNewsRotateAt_ = 0;
  bool renderStateDirty_ = true;
  bool scheduledPrimaryAudioAudible_ = true;
  WorkspaceTab selectedTab_ = WorkspaceTab::Main;
  RECT workspaceBounds_{0, 0, 1, 1};
  inline static App* current_ = nullptr;
};
}  // namespace hp
