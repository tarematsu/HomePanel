#include "secondary_sh.h"
#include "sh_shared.h"

namespace hp {
namespace {
constexpr wchar_t kProfileName[] = L"stationhead-secondary";

bool CallbackAlive(const std::shared_ptr<std::atomic<bool>>& alive) {
  return alive && alive->load(std::memory_order_acquire);
}

std::wstring HResultHex(HRESULT value) {
  std::wostringstream output;
  output << L"0x" << std::hex << static_cast<unsigned long>(value);
  return output.str();
}

}

SecondaryStationheadPlayer::SecondaryStationheadPlayer(
    HWND window, StationheadConfig config, fs::path userDataFolder, Logger& log)
    : window_(window), config_(std::move(config)),
      userDataFolder_(std::move(userDataFolder)), log_(log) {
  status_.url = config_.secondaryUrl;
}

SecondaryStationheadPlayer::~SecondaryStationheadPlayer() {
  callbackAlive_->store(false, std::memory_order_release);
  authCallbackAlive_->store(false, std::memory_order_release);
  Stop();
}

void SecondaryStationheadPlayer::Start() {
  if (shuttingDown_) return;
  retryAt_ = 0;
  nextTickAt_ = 0;
  Create();
}

void SecondaryStationheadPlayer::Stop() {
  if (shuttingDown_.exchange(true, std::memory_order_acq_rel)) return;
  callbackAlive_->store(false, std::memory_order_release);
  authCallbackAlive_->store(false, std::memory_order_release);
  CloseWebView();
  if (authHostWindow_ && IsWindow(authHostWindow_)) DestroyWindow(authHostWindow_);
  if (hostWindow_ && IsWindow(hostWindow_)) DestroyWindow(hostWindow_);
  authHostWindow_ = nullptr;
  hostWindow_ = nullptr;
}

SecondaryStationheadStatus SecondaryStationheadPlayer::Status() const {
  std::lock_guard lock(mutex_);
  SecondaryStationheadStatus copy = status_;
  copy.playing = audioPlaying_.load(std::memory_order_relaxed);
  copy.loginRequired = loginRequired_;
  copy.audioMuted = audioMuted_.load(std::memory_order_relaxed);
  return copy;
}

void SecondaryStationheadPlayer::SetStatus(const std::wstring& detail) {
  std::lock_guard lock(mutex_);
  status_.detail = detail;
}

void SecondaryStationheadPlayer::ApplyPlaybackState(bool playing, const std::wstring& source) {
  const bool changed = audioPlaying_.exchange(playing, std::memory_order_relaxed) != playing;
  if (playing) {
    resourceBlockingArmed_ = true;
    retryAt_ = 0;
    const bool wasLoginInteractive = loginRequired_.exchange(false, std::memory_order_acq_rel);
    if ((wasLoginInteractive || interactive_) && !spotifyAuthorization_) ShowInteractive(false);
    SetStatus(L"audio detected (" + source + L")");
  } else {
    SetStatus(L"audio stopped (" + source + L")");
    if (!spotifyAuthorization_) ShowInteractive(true);
  }
  if (changed) {
    log_.Info(std::wstring(L"Secondary Stationhead audio ") +
              (playing ? L"playing" : L"stopped") + L" (" + source + L")");
    PostMessageW(window_, WM_HP_STATIONHEAD_CHANGED, 0, 0);
  }
}

void SecondaryStationheadPlayer::FinishSpotifyAuthorization(const std::wstring& detail) {
  spotifyAuthorization_ = false;
  authClosePending_ = true;
  SetStatus(detail);
}

void SecondaryStationheadPlayer::ConfigureAuthWebView() {
  const auto alive = callbackAlive_;
  const auto authAlive = authCallbackAlive_;
  appliedMuted_.store(-1, std::memory_order_relaxed);
  appliedVolumePercent_.store(-1, std::memory_order_relaxed);
  ComPtr<ICoreWebView2Settings> settings;
  authWebview_->get_Settings(&settings);
  if (settings) {
    settings->put_AreDefaultContextMenusEnabled(FALSE);
    settings->put_AreDevToolsEnabled(FALSE);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_IsZoomControlEnabled(FALSE);
  }
  authWebview_->add_NavigationCompleted(
      Callback<ICoreWebView2NavigationCompletedEventHandler>(
          [this, alive, authAlive](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            if (!CallbackAlive(alive) || !CallbackAlive(authAlive) || shuttingDown_) return S_OK;
            BOOL success = FALSE;
            if (args) args->get_IsSuccess(&success);
            if (success && authWebview_) {
              ShowInteractive(true);
              authWebview_->PostWebMessageAsJson(L"{\"type\":\"auth-tab-ready\"}");
            }
            return S_OK;
          }).Get(), &authNavigationToken_);
  authWebview_->add_WebMessageReceived(
      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [this, alive, authAlive](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            if (!CallbackAlive(alive) || !CallbackAlive(authAlive) || shuttingDown_) return S_OK;
            LPWSTR raw = nullptr;
            if (!args || FAILED(args->get_WebMessageAsJson(&raw)) || !raw) return S_OK;
            const std::wstring message(raw);
            CoTaskMemFree(raw);
            if (message.find(L"spotify-connected") != std::wstring::npos) {
              FinishSpotifyAuthorization(L"Spotify authentication completed; returning to secondary Stationhead");
            } else if (message.find(L"spotify-error") != std::wstring::npos) {
              FinishSpotifyAuthorization(L"Spotify authentication failed or cancelled");
            }
            return S_OK;
          }).Get(), &authMessageToken_);
  authWebview_->add_WindowCloseRequested(
      Callback<ICoreWebView2WindowCloseRequestedEventHandler>(
          [this, alive, authAlive](ICoreWebView2*, IUnknown*) -> HRESULT {
            if (!CallbackAlive(alive) || !CallbackAlive(authAlive) || shuttingDown_) return S_OK;
            FinishSpotifyAuthorization(L"Spotify login closed; returning to secondary Stationhead");
            return S_OK;
          }).Get(), &authCloseToken_);
  authWebview_->add_ProcessFailed(
      Callback<ICoreWebView2ProcessFailedEventHandler>(
          [this, alive, authAlive](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) -> HRESULT {
            if (!CallbackAlive(alive) || !CallbackAlive(authAlive) || shuttingDown_) return S_OK;
            FinishSpotifyAuthorization(L"Spotify login WebView failed");
            return S_OK;
          }).Get(), &authProcessFailedToken_);
  ApplyAudioState();
}

void SecondaryStationheadPlayer::CloseAuthWebView() {
  authCallbackAlive_->store(false, std::memory_order_release);
  if (authWebview_) {
    if (authNavigationToken_.value) authWebview_->remove_NavigationCompleted(authNavigationToken_);
    if (authMessageToken_.value) authWebview_->remove_WebMessageReceived(authMessageToken_);
    if (authProcessFailedToken_.value) authWebview_->remove_ProcessFailed(authProcessFailedToken_);
    if (authCloseToken_.value) authWebview_->remove_WindowCloseRequested(authCloseToken_);
  }
  authNavigationToken_ = {};
  authMessageToken_ = {};
  authProcessFailedToken_ = {};
  authCloseToken_ = {};
  if (authController_) authController_->Close();
  authWebview_.Reset();
  authController_.Reset();
  appliedMuted_.store(-1, std::memory_order_relaxed);
  appliedVolumePercent_.store(-1, std::memory_order_relaxed);
  if (authHostWindow_ && IsWindow(authHostWindow_)) ShowWindow(authHostWindow_, SW_HIDE);
}

void SecondaryStationheadPlayer::Create() {
  if (shuttingDown_.load(std::memory_order_acquire) || webview_) return;
  if (creating_.exchange(true, std::memory_order_acq_rel)) return;
  if (!EnsureHostWindow()) {
    creating_ = false;
    ScheduleRetry(L"host window unavailable");
    return;
  }
  SetStatus(L"creating isolated WebView2 profile");
  callbackAlive_->store(false, std::memory_order_release);
  callbackAlive_ = std::make_shared<std::atomic<bool>>(true);
  const auto alive = callbackAlive_;
  SharedWebViewEnvironment::Instance().Acquire(
      userDataFolder_, [this, alive](HRESULT result, ICoreWebView2Environment* environment) {
        if (!CallbackAlive(alive)) return;
        if (FAILED(result) || !environment || shuttingDown_) {
          creating_ = false;
          if (!shuttingDown_) ScheduleRetry(L"shared environment unavailable " + HResultHex(result));
          return;
        }
        environment_ = environment;
        ComPtr<ICoreWebView2Environment10> environment10;
        if (FAILED(environment_.As(&environment10)) || !environment10) {
          creating_ = false;
          ScheduleRetry(L"WebView2 multi-profile API unavailable", 30'000);
          return;
        }
        ComPtr<ICoreWebView2ControllerOptions> options;
        HRESULT optionsResult = environment10->CreateCoreWebView2ControllerOptions(options.GetAddressOf());
        if (FAILED(optionsResult) || !options) {
          creating_ = false;
          ScheduleRetry(L"profile options creation failed " + HResultHex(optionsResult));
          return;
        }
        options->put_ProfileName(kProfileName);
        options->put_IsInPrivateModeEnabled(FALSE);
        HRESULT started = environment10->CreateCoreWebView2ControllerWithOptions(
            hostWindow_, options.Get(),
            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [this, alive](HRESULT controllerResult, ICoreWebView2Controller* controller) -> HRESULT {
                  if (!CallbackAlive(alive)) {
                    if (controller) controller->Close();
                    return S_OK;
                  }
                  creating_ = false;
                  if (FAILED(controllerResult) || !controller || shuttingDown_) {
                    if (controller) controller->Close();
                    if (!shuttingDown_) ScheduleRetry(L"profile controller failed " + HResultHex(controllerResult));
                    return S_OK;
                  }
                  controller_ = controller;
                  controller_->get_CoreWebView2(&webview_);
                  if (!webview_) {
                    ScheduleRetry(L"secondary WebView unavailable");
                    return S_OK;
                  }
                  ConfigureWebView();
                  return S_OK;
                }).Get());
        if (FAILED(started)) {
          creating_ = false;
          ScheduleRetry(L"profile controller start failed " + HResultHex(started));
        }
      });
}

void SecondaryStationheadPlayer::Reconnect() {
  if (!webview_) {
    if (!creating_) Create();
    return;
  }
  audioPlaying_ = false;
  loginRequired_ = false;
  retryAt_ = 0;
  SetStartupBounds();
  {
    std::lock_guard lock(mutex_);
    status_.navigating = true;
    status_.playing = false;
    status_.loginRequired = false;
    status_.detail = L"reconnecting secondary station";
  }
  const HRESULT result = webview_->Navigate(config_.secondaryUrl.c_str());
  if (FAILED(result)) ScheduleRetry(L"reconnect navigation failed " + HResultHex(result), 1'000);
}

void SecondaryStationheadPlayer::Tick(int64_t nowMs) {
  if (shuttingDown_) return;
  if (nowMs < nextTickAt_) return;
  if (authClosePending_.exchange(false, std::memory_order_acq_rel)) {
    CloseAuthWebView();
    ShowInteractive(loginRequired_.load(std::memory_order_acquire));
    nextTickAt_ = nowMs + 1'000;
  }
  if (retryAt_ > 0 && nowMs >= retryAt_) {
    retryAt_ = 0;
    CloseWebView();
    Create();
    return;
  }
  if (!webview_) {
    nextTickAt_ = nowMs + 1'000;
    return;
  }
  const int64_t reloadInterval = StationheadReloadIntervalMs(
      std::max(5, config_.secondaryReloadIntervalMinutes));
  if (audioPlaying_.load(std::memory_order_relaxed) && lastReloadAt_ > 0 &&
      nowMs - lastReloadAt_ >= reloadInterval) {
    if (!window_ || !IsWindow(window_) ||
        SendMessageW(window_, WM_HP_SECONDARY_RELOAD_READY, 0, 0) == 0) {
      SetStatus(L"maintenance reload waiting for primary audio");
      nextTickAt_ = nowMs + 30'000;
      return;
    }
    log_.Info(L"Secondary Stationhead 50-minute maintenance reload");
    Reconnect();
    nextTickAt_ = nowMs + 1'000;
    return;
  }
  int64_t next = nowMs + 5 * 60'000;
  const auto consider = [&](int64_t deadline) {
    if (deadline <= nowMs) next = nowMs + 1'000;
    else next = std::min(next, deadline);
  };
  if (retryAt_ > 0) consider(retryAt_);
  if (audioPlaying_.load(std::memory_order_relaxed) && lastReloadAt_ > 0 && reloadInterval > 0) {
    consider(lastReloadAt_ + reloadInterval);
  }
  nextTickAt_ = std::max(nowMs + 1'000, next);
}

void SecondaryStationheadPlayer::ScheduleRetry(const std::wstring& reason, int64_t delayMs) {
  if (shuttingDown_.load(std::memory_order_acquire)) return;
  const int64_t candidate = UnixMillis() + std::max<int64_t>(1'000, delayMs);
  if (retryAt_ == 0 || candidate < retryAt_) retryAt_ = candidate;
  SetStatus(reason + L"; retry scheduled");
  log_.Warn(L"Secondary Stationhead " + reason);
}

void SecondaryStationheadPlayer::CloseWebView() {
  callbackAlive_->store(false, std::memory_order_release);
  authCallbackAlive_->store(false, std::memory_order_release);
  CloseAuthWebView();
  if (webview_) {
    if (audioPlayingChangedToken_.value) {
      ComPtr<ICoreWebView2_8> audioView;
      if (SUCCEEDED(webview_.As(&audioView)) && audioView) {
        audioView->remove_IsDocumentPlayingAudioChanged(audioPlayingChangedToken_);
      }
    }
    if (navigationToken_.value) webview_->remove_NavigationCompleted(navigationToken_);
    if (newWindowToken_.value) webview_->remove_NewWindowRequested(newWindowToken_);
    if (messageToken_.value) webview_->remove_WebMessageReceived(messageToken_);
    if (processFailedToken_.value) webview_->remove_ProcessFailed(processFailedToken_);
    if (resourceRequestedToken_.value) webview_->remove_WebResourceRequested(resourceRequestedToken_);
  }
  audioPlayingChangedToken_ = {};
  navigationToken_ = {};
  newWindowToken_ = {};
  messageToken_ = {};
  processFailedToken_ = {};
  resourceRequestedToken_ = {};
  nativeAudioTracking_ = false;
  resourceBlockingArmed_ = false;
  if (controller_) controller_->Close();
  webview_.Reset();
  controller_.Reset();
  environment_.Reset();
  appliedMuted_.store(-1, std::memory_order_relaxed);
  appliedVolumePercent_.store(-1, std::memory_order_relaxed);
  if (hostWindow_ && IsWindow(hostWindow_)) ShowWindow(hostWindow_, SW_HIDE);
  if (authHostWindow_ && IsWindow(authHostWindow_)) ShowWindow(authHostWindow_, SW_HIDE);
  creating_ = false;
  interactive_ = false;
  spotifyAuthorization_ = false;
  authClosePending_ = false;
  audioPlaying_ = false;
  loginRequired_ = false;
  lastReloadAt_ = 0;
  retryAt_ = 0;
  nextTickAt_ = 0;
  {
    std::lock_guard lock(mutex_);
    status_.created = false;
    status_.navigating = false;
    status_.playing = false;
    status_.loginRequired = false;
    status_.spotifyAuthorization = false;
    status_.visible = false;
    status_.processFailed = false;
  }
}

}




#include "secondary_sh_webview.cpp"
