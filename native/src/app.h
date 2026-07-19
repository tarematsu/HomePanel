#pragma once
#include "app_stationhead_handles.h"
#include "cloud_client.h"
#include "config.h"
#include "logger.h"
#include "render_state.h"
#include "sensors.h"
#include "update_client.h"

namespace hp {

class Renderer;

class App {
 public:
  explicit App(HINSTANCE instance);
  ~App();
  int Run(int showCommand);
  static App* Current();
  void LogUnhandled(DWORD code, void* address);
  void ToggleStationheadAudio();
  void MuteStationheadAudio();

 private:
  static constexpr UINT kUpdateResultMessage = WM_APP + 20;
  static constexpr int kRestartExitCode = 42;
  static void EnrichRenderStationheadState(
      StationheadStatus& state, StationheadStatus* secondaryStatus,
      const StationheadConfig& config);
  static LRESULT CALLBACK WindowProc(
      HWND window, UINT message, WPARAM wParam, LPARAM lParam);
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
  void ApplyStationheadWindowPlacement(
      const StationheadStatus& primaryStatus,
      const StationheadStatus& secondaryStatus);
  void MarkStationheadPlacementDirty() noexcept { stationheadPlacementDirty_ = true; }
  void ProcessRemoteCommands();
  void SendTelemetryAsync();
  void ClearDisplayCache();
  void CheckForUpdateAsync(bool install);
  bool LaunchVerifiedUpdater(
      const std::wstring& version, const std::string& manifestJson);

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
  int exitCode_ = 0;
  int startupShowCommand_ = SW_SHOW;
  int64_t startupAt_ = 0;
  int64_t dashboardAudioReadySince_ = 0;
  int64_t playbackReadyAt_ = 0;
  bool secondaryStarted_ = false;
  bool rendererStarted_ = false;
  bool cloudStarted_ = false;
  bool startupUpdateScheduled_ = false;
  bool stationheadPlaybackFallbackActive_ = false;
  bool stationheadPlaybackNoNextTrackObserved_ = false;
  uint64_t stationheadPlaybackFallbackRevision_ = 0;
  int64_t lastTelemetryAt_ = 0;
  int64_t lastStationheadPlayStatsUpdatedAt_ = 0;
  int64_t lastStationheadPlayHistorySavedAt_ = 0;
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

}  // namespace hp
