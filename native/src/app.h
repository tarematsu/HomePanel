#pragma once
#include "cloud_client.h"
#include "config.h"
#include "logger.h"
#include "web_renderer.h"
#include "sensors.h"
#include "sh.h"
#include "update_client.h"

namespace hp {
class App;

inline bool StationheadNeedsForeground(const StationheadStatus& status) noexcept {
  return !status.audioPlaying;
}

enum class WorkspaceTab {
  Main = 0,
  Stationhead = 1,
  Auth = 2,
};







template <typename Derived, typename PlayerT>
class StationheadHandleBase {
 public:
  explicit operator bool() const noexcept { return static_cast<bool>(player_); }
  Derived* operator->() noexcept { return static_cast<Derived*>(this); }
  const Derived* operator->() const { return static_cast<const Derived*>(this); }

  void Stop() { if (player_) player_->Stop(); }

  void SetAudioMuted(bool muted) noexcept {
    if (audioMuted_ == muted) return;
    audioMuted_ = muted;
    if (player_) player_->SetMuted(muted);
  }
  void ToggleAudioMuted() noexcept { SetAudioMuted(!audioMuted_); }
  bool AudioMuted() const noexcept { return audioMuted_; }
  void SetAudioVolume(double volume) noexcept {
    const double clamped = std::clamp(volume, 0.0, 1.0);
    if (audioVolume_ == clamped) return;
    audioVolume_ = clamped;
    if (player_) player_->SetVolume(clamped);
  }
  double AudioVolume() const noexcept { return audioVolume_; }

  void SetBounds(const RECT& bounds) {
    if (!startupPreviewActive_ && EqualRect(&workspaceBounds_, &bounds)) return;
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

  StationheadStatus Status() const {
    StationheadStatus status = player_ ? player_->Status() : StationheadStatus{};
    const bool forceInteractive = status.loginRequired || status.spotifyAuthorization ||
                                  status.processFailed;
    if (player_ && SuppressTrackTransitionGap(status.audioPlaying, forceInteractive)) {
      if (status.visible) player_->KeepPlaybackBehindDashboard();
      status.audioPlaying = true;
      status.playing = true;
      status.visible = false;
      status.detail = L"track transition; waiting for next audio";
    }
    status.audioMuted = audioMuted_;
    return status;
  }

  int64_t NextWakeAt() const noexcept {
    return player_ ? player_->NextWakeAt() : 0;
  }

  bool IsInteractive(const StationheadStatus& status) const noexcept {
    const bool forceInteractive = status.loginRequired || status.spotifyAuthorization ||
                                  status.processFailed;
    if (forceInteractive) return true;
    return !status.audioPlaying &&
           !SuppressTrackTransitionGap(status.audioPlaying, false);
  }

 protected:
  static constexpr int64_t kTrackTransitionGraceMs = 12'000;

  bool SuppressTrackTransitionGap(bool playing, bool forceInteractive) const noexcept {
    if (playing) {
      playbackObserved_ = true;
      playbackMissingSinceAt_ = 0;
      return false;
    }
    if (forceInteractive || !playbackObserved_) {
      playbackMissingSinceAt_ = 0;
      return false;
    }
    const int64_t now = UnixMillis();
    if (playbackMissingSinceAt_ == 0) playbackMissingSinceAt_ = now;
    return now - playbackMissingSinceAt_ < kTrackTransitionGraceMs;
  }

  void ResetTrackTransitionGrace() noexcept {
    playbackObserved_ = false;
    playbackMissingSinceAt_ = 0;
  }

  void ApplyAudioState() const noexcept {
    if (!player_) return;
    player_->SetMuted(audioMuted_);
    player_->SetVolume(audioVolume_);
  }

  void BringMainWindowToFront(HWND host) const noexcept {
    if (!host || !IsWindow(host)) return;
    HWND root = GetAncestor(host, GA_ROOT);
    if (!root || !IsWindow(root) || GetForegroundWindow() == root) return;
    SetWindowPos(root, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOSENDCHANGING);
    UpdateWindow(root);
  }

  void RaiseActiveHost() const {
    if (!player_) return;
    const bool preview = startupPreviewActive_;
    if (!preview && !player_->SurfaceVisible()) return;
    HWND host = player_->ActiveHostWindowForAccountSetup();
    if (!host || !IsWindow(host)) return;
    bool interactive = false;
    if (!preview) {
      const auto status = player_->Status();
      interactive = IsInteractive(status);
      if (!interactive && !status.visible) return;
    }
    const RECT activeBounds = preview ? startupPreviewBounds_ : workspaceBounds_;
    const int width = std::max(1L, activeBounds.right - activeBounds.left);
    const int height = std::max(1L, activeBounds.bottom - activeBounds.top);
    SetWindowPos(host, HWND_TOP, activeBounds.left, activeBounds.top,
                 width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOSENDCHANGING);
    if (!preview && interactive) BringMainWindowToFront(host);
  }

  void ApplyInteractiveBounds() {
    if (!player_) return;
    player_->ClearStartupPreviewBounds();
    player_->SetBounds(workspaceBounds_);
  }

  void ApplyBounds() {
    if (!player_) return;
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
  mutable bool playbackObserved_ = false;
  mutable int64_t playbackMissingSinceAt_ = 0;
};



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
    ResetTrackTransitionGrace();
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
  }
  void Reconnect() {
    if (!player_) return;
    ApplyInteractiveBounds();
    player_->Reconnect();
    ApplyBounds();
  }
  void SetPlaybackFallback(bool active, const std::wstring& reason) {
    if (!player_) return;
    player_->SetPlaybackFallback(active, reason);
    ApplyBounds();
  }
  void ShowForLogin() {
    if (!player_) return;
    ApplyInteractiveBounds();
    player_->ShowForLogin();
    ApplyBounds();
  }
  void ShowAfterAudioStop() {
    if (!player_) return;
    ApplyInteractiveBounds();
    player_->ShowAfterAudioStop();
    ApplyBounds();
  }
  void OpenSpotifyAuthorization(const std::wstring& url) {
    if (!player_) return;
    ApplyInteractiveBounds();
    player_->OpenSpotifyAuthorization(url);
    ApplyBounds();
  }
  void ReleaseCompletedAuth() {
    if (!player_) return;
    player_->ReleaseCompletedAuth();
    ApplyBounds();
  }
  void ToggleView() {
    if (!player_) return;
    ApplyInteractiveBounds();
    player_->ToggleView();
    ApplyBounds();
  }
  uint32_t ConsumeChangeFlags() {
    return player_ ? player_->ConsumeChangeFlags() : StationheadChangeNone;
  }
  bool HasAuthTab() const { return player_ && player_->HasAuthTab(); }

  void SelectTab(StationheadTabKind tab) {
    selectedTab_ = tab;
    if (!player_) return;
    if (tab == StationheadTabKind::None) {
      const StationheadStatus status = player_->Status();
      const bool forceInteractive = status.loginRequired || status.spotifyAuthorization ||
                                    status.processFailed;
      if (SuppressTrackTransitionGap(status.audioPlaying, forceInteractive)) {
        if (status.visible) player_->KeepPlaybackBehindDashboard();
        return;
      }
    } else {
      ApplyInteractiveBounds();
    }
    player_->SelectTab(tab);
    ApplyBounds();
  }
 private:
  StationheadTabKind selectedTab_ = StationheadTabKind::None;
};



class AppSecondaryStationheadHandle
    : public StationheadHandleBase<AppSecondaryStationheadHandle, StationheadPlayer> {
 public:
  AppSecondaryStationheadHandle() = default;
  AppSecondaryStationheadHandle(const AppSecondaryStationheadHandle&) = delete;
  AppSecondaryStationheadHandle& operator=(const AppSecondaryStationheadHandle&) = delete;

  AppSecondaryStationheadHandle& operator=(std::unique_ptr<StationheadPlayer> player) noexcept {
    player_ = std::move(player);
    ApplyAudioState();
    ApplyBounds();
    return *this;
  }

  void reset() noexcept {
    player_.reset();
    ResetTrackTransitionGrace();
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
  }
  void Reconnect() {
    if (!player_) return;
    ApplyInteractiveBounds();
    player_->Reconnect();
    ApplyBounds();
  }
  void SetPlaybackFallback(bool active, const std::wstring& reason) {
    if (!player_) return;
    player_->SetPlaybackFallback(active, reason);
    ApplyBounds();
  }
};

class App {
 public:
  explicit App(HINSTANCE instance);
  ~App();
  int Run(int showCommand);
  static App* Current();
  void LogUnhandled(DWORD code, void* address);
  void ToggleStationheadAudio() {
    const bool primaryAudible = secondaryStationhead_
        ? !scheduledPrimaryAudioAudible_
        : true;
    stationheadAudioMuted_ = false;
    ApplyScheduledStationheadAudioProfile(primaryAudible);
    renderState_.toast = primaryAudible ? L"A 音声ON" : L"B 音声ON";
    toastUntil_ = UnixMillis() + 3000;
    MarkRenderStateDirty();
    InvalidateAll();
  }
  void MuteStationheadAudio() {
    stationheadAudioMuted_ = true;
    ApplyScheduledStationheadAudioProfile(scheduledPrimaryAudioAudible_);
    renderState_.toast = L"MUTE";
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
  bool UpdateRenderStationheadState(StationheadStatus nextState);
  void ScheduleNextTick(uint32_t milliseconds);
  void ApplyScheduledStationheadAudioProfile(bool primaryAudible) noexcept;
  void UpdateStationheadPlaybackFallback(int64_t nowMs);
  void PublishRenderState();
  void PublishRenderStateNow();
  void InvalidateAll();
  void LoadAirHistory();
  void SaveAirHistory() const;
  void UpdateAirHistory(const SensorSnapshot& sensors);
  void LoadStationheadPlayHistory();
  void SaveStationheadPlayHistory() const;
  void UpdateStationheadPlayHistory(const StationheadStatus& status);
  void HandleAction(UiAction action);
  void LayoutWorkspace();
  void ApplyStationheadWindowPlacement(const StationheadStatus& primaryStatus,
                                       const StationheadStatus& secondaryStatus);
  void MarkStationheadPlacementDirty() noexcept { stationheadPlacementDirty_ = true; }
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
  bool stationheadPlaybackFallbackActive_ = false;
  bool stationheadPlaybackNoNextTrackObserved_ = false;
  uint64_t stationheadPlaybackFallbackRevision_ = 0;
  int64_t lastTelemetryAt_ = 0;
  uint64_t lastRadarFrameStamp_ = 0;
  int64_t toastUntil_ = 0;
  int64_t nextAppTickAt_ = 0;

  int newsIndex_ = 0;
  int newsCount_ = 0;
  int64_t lastNewsRotateAt_ = 0;
  bool renderStateDirty_ = true;
  bool stationheadPlacementDirty_ = true;
  bool placedPrimaryPending_ = false;
  bool placedSecondaryPending_ = false;
  RECT placedBounds_{};
  bool scheduledPrimaryAudioAudible_ = true;
  bool stationheadAudioMuted_ = false;
  WorkspaceTab selectedTab_ = WorkspaceTab::Main;
  RECT workspaceBounds_{0, 0, 1, 1};
  inline static App* current_ = nullptr;
};
}
