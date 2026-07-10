#pragma once
#include "common.h"
#include "config.h"
#include "logger.h"
#include "shared_webview_environment.h"

namespace hp {

struct SecondaryStationheadStatus {
  bool created = false;
  bool navigating = false;
  bool playing = false;
  bool loginRequired = false;
  bool spotifyAuthorization = false;
  bool visible = false;
  bool processFailed = false;
  bool audioMuted = false;
  std::wstring detail;
  std::wstring url;
};

class SecondaryStationheadPlayer {
 public:
  SecondaryStationheadPlayer(HWND window, StationheadConfig config,
                             fs::path userDataFolder, Logger& log);
  SecondaryStationheadPlayer(const SecondaryStationheadPlayer&) = delete;
  SecondaryStationheadPlayer& operator=(const SecondaryStationheadPlayer&) = delete;
  SecondaryStationheadPlayer(SecondaryStationheadPlayer&&) = delete;
  SecondaryStationheadPlayer& operator=(SecondaryStationheadPlayer&&) = delete;
  ~SecondaryStationheadPlayer();

  void Start();
  void Stop();
  void Tick(int64_t nowMs);
  [[nodiscard]] int64_t NextWakeAt() const noexcept { return nextTickAt_; }
  void Reconnect();
  void SetBounds(const RECT& bounds);
  void SetStartupPreviewBounds(const RECT& bounds);
  void ClearStartupPreviewBounds();
  [[nodiscard]] SecondaryStationheadStatus Status() const;
  HWND ActiveHostWindowForAccountSetup() const noexcept {
    if (spotifyAuthorization_ && authHostWindow_ && IsWindow(authHostWindow_)) return authHostWindow_;
    return hostWindow_;
  }

  void SetMuted(bool muted) noexcept;
  bool Muted() const noexcept;
  void SetVolume(double volume) noexcept;
  double Volume() const noexcept;

 private:
  void ApplyAudioState() noexcept;
  void ApplyVolume() const noexcept;
  void ApplyPlaybackState(bool playing, const std::wstring& source);
  void EnsureDistinctBrowserIdentity() noexcept;

  void Create();
  void ConfigureWebView();
  void ConfigureAuthWebView();
  void CloseWebView();
  void CloseAuthWebView();
  bool EnsureHostWindow();
  bool EnsureAuthHostWindow();
  void SetStartupBounds();
  void LayoutWindows(bool interactive);
  void ShowInteractive(bool interactive);
  void FinishSpotifyAuthorization(const std::wstring& detail);
  void ScheduleRetry(const std::wstring& reason, int64_t delayMs = 5'000);
  void SetStatus(const std::wstring& detail);

  HWND window_{};
  HWND hostWindow_{};
  HWND authHostWindow_{};
  RECT bounds_{0, 0, 1, 1};
  StationheadConfig config_;
  fs::path userDataFolder_;
  Logger& log_;
  mutable std::mutex mutex_;
  SecondaryStationheadStatus status_;
  ComPtr<ICoreWebView2Environment> environment_;
  ComPtr<ICoreWebView2Controller> controller_;
  ComPtr<ICoreWebView2> webview_;
  ComPtr<ICoreWebView2Controller> authController_;
  ComPtr<ICoreWebView2> authWebview_;
  EventRegistrationToken navigationToken_{};
  EventRegistrationToken newWindowToken_{};
  EventRegistrationToken messageToken_{};
  EventRegistrationToken processFailedToken_{};
  EventRegistrationToken resourceRequestedToken_{};
  EventRegistrationToken audioPlayingChangedToken_{};
  std::atomic<bool> resourceBlockingArmed_{false};
  EventRegistrationToken authNavigationToken_{};
  EventRegistrationToken authMessageToken_{};
  EventRegistrationToken authProcessFailedToken_{};
  EventRegistrationToken authCloseToken_{};
  std::shared_ptr<std::atomic<bool>> callbackAlive_{std::make_shared<std::atomic<bool>>(true)};
  std::shared_ptr<std::atomic<bool>> authCallbackAlive_{std::make_shared<std::atomic<bool>>(false)};
  std::atomic<bool> creating_{false};
  std::atomic<bool> shuttingDown_{false};
  std::atomic<bool> audioPlaying_{false};
  std::atomic<bool> authClosePending_{false};
  std::atomic<bool> audioMuted_{false};
  std::atomic<double> audioVolume_{1.0};
  // Last mute/volume actually pushed into the WebViews (-1 = never pushed).
  // ApplyAudioState/ApplyVolume run on every 1s app tick via ApplyBounds;
  // without this cache each tick would re-run ExecuteScript in the browser
  // process.
  std::atomic<int> appliedMuted_{-1};
  mutable std::atomic<int> appliedVolumePercent_{-1};
  bool nativeAudioTracking_ = false;
  bool interactive_ = false;
  bool startupPreviewActive_ = false;
  bool spotifyAuthorization_ = false;
  std::atomic<bool> loginRequired_{false};
  ICoreWebView2* identityWebview_ = nullptr;
  int64_t lastReloadAt_ = 0;
  int64_t nextTickAt_ = 0;
  int64_t retryAt_ = 0;
};
}  // namespace hp
