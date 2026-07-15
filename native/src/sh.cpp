#include "sh.h"
#include "shared_webview_environment.h"
#include "sh_shared.h"

namespace hp {
namespace {
constexpr int64_t kDailyPlayStatsIntervalMs = 5 * 60'000;
constexpr int64_t kAuthProbeIntervalMs = 5 * 60'000;
constexpr int64_t kAuthProbeTimeoutMs = 30'000;

bool CallbackAlive(const std::shared_ptr<std::atomic<bool>>& alive) {
  return alive && alive->load(std::memory_order_acquire);
}

std::wstring HResultHex(HRESULT hr) {
  std::wostringstream output;
  output << L"0x" << std::hex << std::setw(8) << std::setfill(L'0')
         << static_cast<unsigned long>(hr);
  return output.str();
}
}

StationheadPlayer::StationheadPlayer(StationheadRole role, HWND window, StationheadConfig config,
                                     fs::path userDataFolder, Logger& log)
    : role_(role), window_(window), config_(std::move(config)),
      userDataFolder_(std::move(userDataFolder)), log_(log) {
  if (IsSecondary()) status_.url = config_.secondaryUrl;
}

StationheadPlayer::~StationheadPlayer() { Stop(); }

void StationheadPlayer::Start() {
  shuttingDown_ = false;
  usingFallback_ = false;
  ResetNavigationRouteState();
  Create();
}

void StationheadPlayer::Stop() {
  shuttingDown_ = true;
  createCallbackAlive_->store(false, std::memory_order_release);
  authCallbackAlive_->store(false, std::memory_order_release);
  CloseAuthWebView();
  CloseWebView();
  if (authHostWindow_ && IsWindow(authHostWindow_)) DestroyWindow(authHostWindow_);
  if (hostWindow_ && IsWindow(hostWindow_)) DestroyWindow(hostWindow_);
  authHostWindow_ = nullptr;
  hostWindow_ = nullptr;
}

StationheadStatus StationheadPlayer::Status() const {
  std::lock_guard lock(mutex_);
  StationheadStatus copy = status_;
  copy.authAvailable = authController_ != nullptr || !authPendingUrl_.empty();
  copy.spotifyAuthorization = spotifyAuthorization_;
  copy.audioPlaying = audioPlaying_.load(std::memory_order_relaxed);
  copy.playing = copy.audioPlaying;
  copy.audioMuted = audioMuted_.load(std::memory_order_relaxed);
  return copy;
}

void StationheadPlayer::ResetNavigationRouteState() {
  audioPlaying_ = false;
  nextTickAt_ = 0;
}

void StationheadPlayer::ApplyAudioPlaybackState(bool playing, const std::wstring& source) {
  const bool changed =
      audioPlaying_.exchange(playing, std::memory_order_relaxed) != playing;
  if (playing) {
    resourceBlockingArmed_ = true;
    loginRequired_ = false;
    {
      std::lock_guard lock(mutex_);
      status_.audioPlaying = true;
      status_.playing = true;
      status_.loginRequired = false;
      status_.navigating = false;
      status_.detail = (usingFallback_ ? L"fallback audio detected" : L"audio detected") +
                       (source.empty() ? L"" : L" (" + source + L")");
    }
    if (!startupPreviewActive_ && !spotifyAuthorization_) SetVisible(false);
    if (changed) log_.Info(L"Stationhead audio playing (" + source + L")");
    PostChange(StationheadChangeReturnMain);
    return;
  }

  // Un-arm stylesheet blocking while audio isn't playing: Stationhead can
  // briefly stop the audio element between tracks and needs to load styles
  // for a Resume/Continue control, and a stale "ever played" latch would
  // keep that CSS blocked until the next full navigation.
  resourceBlockingArmed_ = false;
  {
    std::lock_guard lock(mutex_);
    status_.audioPlaying = false;
    status_.playing = false;
    status_.detail = usingFallback_ ? L"fallback audio stopped" : L"audio stopped";
  }
  nextTickAt_ = 0;
  if (changed) log_.Warn(L"Stationhead audio stopped (" + source + L")");
  if (!spotifyAuthorization_) SelectTab(StationheadTabKind::None);
  PostChange();
}

void StationheadPlayer::NavigateCurrentUrl(int64_t nowMs, const std::wstring& reason) {
  NavigateStationheadUrl(nowMs, CurrentStationheadUrl(), reason, usingFallback_);
}

std::wstring StationheadPlayer::CurrentStationheadUrl() const {
  if (usingFallback_ && !config_.fallbackUrl.empty()) return config_.fallbackUrl;
  return IsSecondary() ? config_.secondaryUrl : config_.url;
}

void StationheadPlayer::SetPlaybackFallback(bool active, const std::wstring& reason) {
  if (active && config_.fallbackUrl.empty()) return;
  if (usingFallback_ == active) return;
  usingFallback_ = active;
  const std::wstring url = CurrentStationheadUrl();
  if (!webview_) {
    std::lock_guard lock(mutex_);
    status_.url = url;
    status_.detail = reason;
    PostChange();
    return;
  }
  NavigateStationheadUrl(UnixMillis(), url, reason, active);
  PostChange();
}

void StationheadPlayer::NavigateStationheadUrl(int64_t nowMs, const std::wstring& url,
                                               const std::wstring& reason,
                                               bool fallbackActive) {
  (void)nowMs;
  if (!webview_ || url.empty()) return;
  SetStartupBounds();
  ResetNavigationRouteState();
  usingFallback_ = fallbackActive;
  resourceBlockingArmed_ = false;
  loginRequired_ = false;
  lastAuthProbeAt_ = 0;
  authProbeStartedAt_ = 0;
  authProbeInFlight_ = false;
  {
    std::lock_guard lock(mutex_);
    status_.navigating = true;
    status_.loginRequired = false;
    status_.audioPlaying = false;
    status_.playing = false;
    status_.url = url;
    status_.detail = reason;
  }
  const HRESULT result = webview_->Navigate(url.c_str());
  if (FAILED(result)) {
    ScheduleRecreate(L"navigation start failed " + HResultHex(result), 1'000);
    return;
  }
  log_.Info(L"Stationhead navigation (" + reason + L"): " + url);
}

void StationheadPlayer::PollDailyPlayStats(int64_t nowMs) {
  if (!webview_) return;
  lastDailyPlayStatsAt_ = nowMs;
  const std::wstring script = StationheadApiPlayStatsScript(config_.channelId);
  webview_->ExecuteScript(script.c_str(), nullptr);
}

void StationheadPlayer::PollAuthProbe(int64_t nowMs) {
  if (!webview_ || spotifyAuthorization_ || loginRequired_ || authProbeInFlight_) return;
  authProbeInFlight_ = true;
  authProbeStartedAt_ = nowMs;
  lastAuthProbeAt_ = nowMs;
  const HRESULT result = webview_->ExecuteScript(
      StationheadAuthProbeScript(config_.channelId).c_str(), nullptr);
  if (FAILED(result)) {
    authProbeInFlight_ = false;
    authProbeStartedAt_ = 0;
    log_.Warn(L"Secondary Stationhead auth probe could not start " + HResultHex(result));
  }
}

void StationheadPlayer::Create() {
  if (shuttingDown_ || creating_.exchange(true)) return;
  if (!EnsureHostWindow()) {
    creating_ = false;
    ScheduleRecreate(L"main window unavailable");
    return;
  }
  createCallbackAlive_->store(false, std::memory_order_release);
  createCallbackAlive_ = std::make_shared<std::atomic<bool>>(true);
  const auto alive = createCallbackAlive_;
  SharedWebViewEnvironment::Instance().Acquire(
      userDataFolder_, [this, alive](HRESULT result, ICoreWebView2Environment* environment) {
        if (!CallbackAlive(alive)) return;
        if (FAILED(result) || !environment || shuttingDown_) {
          creating_ = false;
          if (!shuttingDown_) {
            ScheduleRecreate(L"shared environment acquisition failed " + HResultHex(result));
          }
          return;
        }
        environment_ = environment;
        const auto onController = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [this, alive](HRESULT controllerResult,
                   ICoreWebView2Controller* controller) -> HRESULT {
              if (!CallbackAlive(alive)) {
                if (controller) controller->Close();
                return S_OK;
              }
              creating_ = false;
              if (FAILED(controllerResult) || !controller || shuttingDown_) {
                if (controller) controller->Close();
                if (!shuttingDown_) {
                  ScheduleRecreate(L"controller creation failed " +
                                   HResultHex(controllerResult));
                }
                return S_OK;
              }
              controller_ = controller;
              controller_->put_IsVisible(FALSE);
              controller_->get_CoreWebView2(&webview_);
              if (!webview_) {
                ScheduleRecreate(L"WebView unavailable after controller creation");
                return S_OK;
              }
              ConfigureWebView();
              return S_OK;
            });
        HRESULT started = E_FAIL;
        if (!IsSecondary()) {
          started = environment_->CreateCoreWebView2Controller(hostWindow_, onController.Get());
        } else {
          ComPtr<ICoreWebView2Environment10> environment10;
          if (FAILED(environment_.As(&environment10)) || !environment10) {
            creating_ = false;
            ScheduleRecreate(L"WebView2 multi-profile API unavailable", 30'000);
            return;
          }
          ComPtr<ICoreWebView2ControllerOptions> options;
          const HRESULT optionsResult =
              environment10->CreateCoreWebView2ControllerOptions(options.GetAddressOf());
          if (FAILED(optionsResult) || !options) {
            creating_ = false;
            ScheduleRecreate(L"profile options creation failed " + HResultHex(optionsResult));
            return;
          }
          options->put_ProfileName(L"stationhead-secondary");
          options->put_IsInPrivateModeEnabled(FALSE);
          started = environment10->CreateCoreWebView2ControllerWithOptions(
              hostWindow_, options.Get(), onController.Get());
        }
        if (FAILED(started)) {
          creating_ = false;
          ScheduleRecreate(L"controller creation could not start " + HResultHex(started));
        }
      });
}

void StationheadPlayer::EnsureAuthController(const std::wstring& url) {
  authPendingUrl_ = url;
  if (!environment_ || authController_ || !EnsureAuthHostWindow()) return;
  authCallbackAlive_->store(false, std::memory_order_release);
  authCallbackAlive_ = std::make_shared<std::atomic<bool>>(true);
  const auto alive = authCallbackAlive_;
  const auto onController = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
      [this, alive](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
        if (!CallbackAlive(alive)) {
          if (controller) controller->Close();
          return S_OK;
        }
        if (FAILED(result) || !controller || shuttingDown_) {
          if (controller) controller->Close();
          return S_OK;
        }
        authController_ = controller;
        authController_->put_IsVisible(FALSE);
        authController_->get_CoreWebView2(&authWebview_);
        ConfigureAuthWebView();
        return S_OK;
      });
  if (!IsSecondary()) {
    environment_->CreateCoreWebView2Controller(authHostWindow_, onController.Get());
    return;
  }
  ComPtr<ICoreWebView2Environment10> environment10;
  if (FAILED(environment_.As(&environment10)) || !environment10) return;
  ComPtr<ICoreWebView2ControllerOptions> options;
  if (FAILED(environment10->CreateCoreWebView2ControllerOptions(options.GetAddressOf())) || !options) return;
  options->put_ProfileName(L"stationhead-secondary");
  options->put_IsInPrivateModeEnabled(FALSE);
  environment10->CreateCoreWebView2ControllerWithOptions(authHostWindow_, options.Get(), onController.Get());
}

void StationheadPlayer::Tick(int64_t nowMs) {
  if (shuttingDown_) return;
  if (nowMs < nextTickAt_ && !(recreating_.load(std::memory_order_relaxed) && nowMs >= recreateAt_)) {
    return;
  }
  nextTickAt_ = nowMs + 60'000;
  if (recreating_.load(std::memory_order_relaxed) && nowMs >= recreateAt_) {
    recreating_ = false;
    recreateAt_ = 0;
    CloseWebView();
    Create();
    nextTickAt_ = nowMs + 1'000;
    return;
  }
  if (!webview_) {
    if (!creating_ && nowMs - createdAt_ > 5'000) Create();
    nextTickAt_ = nowMs + 1'000;
    return;
  }
  if (!spotifyAuthorization_ && authController_) CloseAuthWebView();
  if (spotifyAuthorization_ || loginRequired_) {
    nextTickAt_ = nowMs + 1'000;
    return;
  }

  if (nowMs - lastReloadAt_ >= kStationheadSessionRefreshIntervalMs) {
    lastReloadAt_ = nowMs;
    NavigateCurrentUrl(nowMs, L"periodic authentication refresh");
    nextTickAt_ = nowMs + 1'000;
    return;
  }

  int64_t next = nowMs + 30 * 60'000;
  const auto consider = [&](int64_t deadline) {
    if (deadline <= nowMs) next = nowMs + 1'000;
    else next = std::min(next, deadline);
  };
  if (!IsSecondary()) {
    if (nowMs - lastDailyPlayStatsAt_ >= kDailyPlayStatsIntervalMs) PollDailyPlayStats(nowMs);
    consider(lastDailyPlayStatsAt_ + kDailyPlayStatsIntervalMs);
  } else {
    if (authProbeInFlight_ && nowMs - authProbeStartedAt_ >= kAuthProbeTimeoutMs) {
      authProbeInFlight_ = false;
      authProbeStartedAt_ = 0;
      log_.Warn(L"Secondary Stationhead auth probe timed out");
    }
    if (lastAuthProbeAt_ > 0 && nowMs - lastAuthProbeAt_ >= kAuthProbeIntervalMs) {
      PollAuthProbe(nowMs);
    }
    if (authProbeInFlight_) consider(authProbeStartedAt_ + kAuthProbeTimeoutMs);
    if (lastAuthProbeAt_ > 0 && !authProbeInFlight_) consider(lastAuthProbeAt_ + kAuthProbeIntervalMs);
  }
  nextTickAt_ = std::max(nowMs + 1'000, next);
}

void StationheadPlayer::Reconnect() { ScheduleRecreate(L"manual reconnect"); }

void StationheadPlayer::OpenSpotifyAuthorization(const std::wstring& url) {
  if (url.empty()) return;
  if (!webview_) {
    pendingAuthorizationUrl_ = url;
    if (!creating_) ScheduleRecreate(L"Spotify authorization requested before WebView2 was ready");
    PostChange();
    return;
  }
  spotifyAuthorization_ = true;
  loginRequired_ = false;
  EnsureAuthController(url);
  SelectTab(StationheadTabKind::Auth);
  {
    std::lock_guard lock(mutex_);
    status_.navigating = true;
    status_.detail = L"Spotify login loading";
  }
  if (authWebview_) authWebview_->Navigate(url.c_str());
  PostChange();
}

void StationheadPlayer::FinishSpotifyAuthorization(const std::wstring& detail) {
  spotifyAuthorization_ = false;
  {
    std::lock_guard lock(mutex_);
    status_.detail = detail;
  }
  SelectTab(StationheadTabKind::None);
  PostChange(StationheadChangeReturnMain | StationheadChangeReleaseAuth);
}

void StationheadPlayer::ShowForLogin() {
  SelectTab(StationheadTabKind::Stationhead);
  log_.Warn(L"Stationhead login required; Stationhead window visible");
}

void StationheadPlayer::ShowAfterAudioStop() {
  if (!webview_) return;
  selectedTab_ = StationheadTabKind::Stationhead;
  viewVisible_ = true;
  nextTickAt_ = 0;
  {
    std::lock_guard lock(mutex_);
    status_.detail = L"Stationhead audio stopped; player restored";
  }
  SetVisible(true);
  log_.Warn(L"Stationhead audio stopped; restored the player");
}

void StationheadPlayer::ReleaseCompletedAuth() {
  if (!spotifyAuthorization_ && authController_) CloseAuthWebView();
}

void StationheadPlayer::ToggleView() {
  if (!controller_) return;
  bool loginRequiredNow = false;
  {
    std::lock_guard lock(mutex_);
    loginRequiredNow = status_.loginRequired;
  }
  if (spotifyAuthorization_ || loginRequiredNow) {
    SelectTab(selectedTab_ == StationheadTabKind::Auth ? StationheadTabKind::Auth
                                                       : StationheadTabKind::Stationhead);
    return;
  }
  SetVisible(!viewVisible_);
  PostChange();
}

uint32_t StationheadPlayer::ConsumeChangeFlags() {
  const uint32_t flags = pendingChangeFlags_.exchange(0, std::memory_order_acq_rel);
  changeMessagePending_ = false;
  if (pendingChangeFlags_.load(std::memory_order_acquire) != 0 &&
      !changeMessagePending_.exchange(true, std::memory_order_acq_rel) &&
      window_ && IsWindow(window_)) {
    PostMessageW(window_, WM_HP_STATIONHEAD_CHANGED, 0, 0);
  }
  return flags;
}

void StationheadPlayer::PostChange(uint32_t flags) {
  pendingChangeFlags_.fetch_or(flags, std::memory_order_release);
  if (changeMessagePending_.exchange(true, std::memory_order_acq_rel)) return;
  if (!window_ || !IsWindow(window_) || !PostMessageW(window_, WM_HP_STATIONHEAD_CHANGED, 0, 0)) {
    changeMessagePending_ = false;
  }
}

void StationheadPlayer::ScheduleRecreate(const std::wstring& reason, int64_t delayMs) {
  if (shuttingDown_) return;
  const int64_t candidate = UnixMillis() + std::max<int64_t>(0, delayMs);
  const bool wasRecreating = recreating_.exchange(true);
  if (!wasRecreating || candidate < recreateAt_) recreateAt_ = candidate;
  nextTickAt_ = 0;
  {
    std::lock_guard lock(mutex_);
    status_.detail = L"recreate scheduled: " + reason;
  }
  log_.Warn(L"Stationhead WebView recreate scheduled: " + reason);
  PostChange();
}

}  // namespace hp
