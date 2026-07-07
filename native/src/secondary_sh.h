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
  bool lightweight = false;
  bool loginRequired = false;
  bool spotifyAuthorization = false;
  bool apiAuthorization = false;
  bool visible = false;
  bool processFailed = false;
  bool audioMuted = false;
  std::wstring detail;
  std::wstring url;
};

class SecondaryStationheadPlayer;
void MaybeStartSpotifyApiAuthorization(SecondaryStationheadPlayer* player) noexcept;

struct SecondaryStationheadTimestamp {
  int64_t value = 0;

  SecondaryStationheadTimestamp& operator=(int64_t next) noexcept {
    value = next;
    return *this;
  }
  operator int64_t() const noexcept { return value; }
  bool operator==(int other) const noexcept { return value == other; }
  bool operator>(int) const noexcept { return false; }
};

inline int64_t operator-(int64_t lhs, const SecondaryStationheadTimestamp& rhs) noexcept {
  return lhs - rhs.value;
}

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
  void Reconnect();
  bool OpenSpotifyApiAuthorization(const std::wstring& url);
  void SetBounds(const RECT& bounds);
  [[nodiscard]] SecondaryStationheadStatus Status() const;
  [[nodiscard]] bool HasLocalSpotifyToken() const noexcept {
    std::error_code error;
    return fs::exists(userDataFolder_.parent_path() / L"spotify-token.dat", error);
  }
  HWND ActiveHostWindowForAccountSetup() const noexcept {
    if (spotifyAuthorization_ && !apiAuthorization_ && authHostWindow_ && IsWindow(authHostWindow_)) return authHostWindow_;
    return hostWindow_;
  }

  void SetMuted(bool muted) noexcept;
  bool Muted() const noexcept;
  void EnsureMuted() noexcept;
  void EnsureAudioState() noexcept;
  void SetVolume(double volume) noexcept;
  double Volume() const noexcept;

 private:
  friend void MaybeStartSpotifyApiAuthorization(SecondaryStationheadPlayer* player) noexcept;

  void ApplyAudioState() noexcept;
  void ApplyVolume() const noexcept;
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
  void ResetSpotifyApiAuthorization(const std::wstring& detail,
                                    bool allowAutomaticRetry = false);
  void PollSpotifyApiAuthorization();
  void StartSpotifyApiTokenExchange(const std::wstring& target);
  void RestoreSecondaryAfterSpotifyApiAuthorization();
  void StopSpotifyApiAuthorizationWorker() noexcept;
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
  std::atomic<bool> resourceBlockingArmed_{false};
  EventRegistrationToken authNavigationToken_{};
  EventRegistrationToken authMessageToken_{};
  EventRegistrationToken authProcessFailedToken_{};
  EventRegistrationToken authCloseToken_{};
  EventRegistrationToken apiAuthMessageToken_{};
  EventRegistrationToken apiAuthNavigationStartingToken_{};
  EventRegistrationToken apiAuthNavigationCompletedToken_{};
  EventRegistrationToken apiAuthProcessFailedToken_{};
  std::shared_ptr<std::atomic<bool>> callbackAlive_{std::make_shared<std::atomic<bool>>(true)};
  std::shared_ptr<std::atomic<bool>> authCallbackAlive_{std::make_shared<std::atomic<bool>>(false)};
  std::atomic<bool> creating_{false};
  std::atomic<bool> shuttingDown_{false};
  std::atomic<bool> audioPlaying_{false};
  std::atomic<bool> authClosePending_{false};
  std::atomic<bool> audioMuted_{false};
  std::atomic<double> audioVolume_{1.0};
  bool interactive_ = false;
  bool spotifyAuthorization_ = false;
  bool apiAuthorization_ = false;
  std::atomic<bool> loginRequired_{false};
  ICoreWebView2* identityWebview_ = nullptr;
  SecondaryStationheadTimestamp createdAt_;
  int64_t lastAudioAt_ = 0;
  SecondaryStationheadTimestamp audioStoppedAt_;
  int64_t lastReloadAt_ = 0;
  int64_t nextTickAt_ = 0;
  int64_t retryAt_ = 0;
  int64_t apiAuthStartedAt_ = 0;
  std::mutex apiAuthResultMutex_;
  std::atomic<bool> apiAuthExchangePending_{false};
  std::atomic<bool> apiAuthExchangeDone_{false};
  bool apiAuthExchangeSucceeded_ = false;
  std::wstring apiAuthExchangeDetail_;
  std::jthread apiAuthThread_;
};
}  // namespace hp
