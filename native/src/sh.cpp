#include "sh.h"
#include "shared_webview_environment.h"
#include "sh_shared.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {
constexpr int64_t kPrimaryNoAudioFallbackMs = 360'000;

std::wstring HResultHex(HRESULT hr) {
  std::wostringstream output;
  output << L"0x" << std::hex << std::setw(8) << std::setfill(L'0')
         << static_cast<unsigned long>(hr);
  return output.str();
}
}  // namespace

StationheadPlayer::StationheadPlayer(HWND window, StationheadConfig config,
                                     fs::path userDataFolder, Logger& log)
    : window_(window), config_(std::move(config)),
      userDataFolder_(std::move(userDataFolder)), log_(log) {
  bounds_ = RECT{0, 0, 1, 1};
}

StationheadPlayer::~StationheadPlayer() { Stop(); }

void StationheadPlayer::Start() {
  shuttingDown_ = false;
  usedFallback_ = false;
  ResetNavigationRouteState();
  Create();
}

void StationheadPlayer::Stop() {
  shuttingDown_ = true;
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

void StationheadPlayer::NavigatePrimaryUrl(int64_t nowMs, const std::wstring& reason) {
  NavigateStationheadUrl(nowMs, config_.url, reason, false);
}

void StationheadPlayer::NavigateStationheadUrl(int64_t nowMs, const std::wstring& url,
                                               const std::wstring& reason,
                                               bool fallbackActive) {
  if (!webview_ || url.empty()) return;
  SetStartupBounds();
  ResetNavigationRouteState();
  usedFallback_ = fallbackActive;
  noAudioSinceAt_ = fallbackActive ? 0 : nowMs;
  resourceBlockingArmed_ = false;
  loginSessionActive_ = false;
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
    ScheduleRecreate(L"navigation start failed " + HResultHex(result));
    return;
  }
  log_.Info(L"Stationhead navigation (" + reason + L"): " + url);
}

void StationheadPlayer::Create() {
  if (shuttingDown_ || creating_.exchange(true)) return;
  if (!EnsureHostWindow()) {
    creating_ = false;
    ScheduleRecreate(L"main window unavailable");
    return;
  }
  SharedWebViewEnvironment::Instance().Acquire(
      userDataFolder_, [this](HRESULT result, ICoreWebView2Environment* environment) {
        if (FAILED(result) || !environment || shuttingDown_) {
          creating_ = false;
          if (!shuttingDown_) {
            ScheduleRecreate(L"shared environment acquisition failed " + HResultHex(result));
          }
          return;
        }
        environment_ = environment;
        const HRESULT started = environment_->CreateCoreWebView2Controller(
            hostWindow_, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                             [this](HRESULT controllerResult,
                                    ICoreWebView2Controller* controller) -> HRESULT {
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
                               ConfigureWebView();
                               return S_OK;
                             }).Get());
        if (FAILED(started)) {
          creating_ = false;
          ScheduleRecreate(L"controller creation could not start " + HResultHex(started));
        }
      });
}

void StationheadPlayer::EnsureAuthController(const std::wstring& url) {
  authPendingUrl_ = url;
  if (!environment_ || authController_ || !EnsureAuthHostWindow()) return;
  environment_->CreateCoreWebView2Controller(
      authHostWindow_, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                           [this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                             if (FAILED(result) || !controller || shuttingDown_) {
                               if (controller) controller->Close();
                               return S_OK;
                             }
                             authController_ = controller;
                             authController_->put_IsVisible(FALSE);
                             authController_->get_CoreWebView2(&authWebview_);
                             ConfigureAuthWebView();
                             return S_OK;
                           }).Get());
}

void StationheadPlayer::ConfigureWebView() {
  // Fresh WebView: force the next ApplyMute/ApplyVolume to actually push state.
  appliedMuted_.store(-1, std::memory_order_relaxed);
  appliedVolumePercent_.store(-1, std::memory_order_relaxed);
  SetStartupBounds();
  ComPtr<ICoreWebView2Controller2> controller2;
  if (SUCCEEDED(controller_.As(&controller2))) {
    COREWEBVIEW2_COLOR background{255, 7, 17, 28};
    controller2->put_DefaultBackgroundColor(background);
  }
  ComPtr<ICoreWebView2Settings> settings;
  webview_->get_Settings(&settings);
  if (settings) {
    settings->put_AreDefaultContextMenusEnabled(FALSE);
    settings->put_AreDevToolsEnabled(FALSE);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_IsZoomControlEnabled(FALSE);
    ComPtr<ICoreWebView2Settings3> settings3;
    if (SUCCEEDED(settings.As(&settings3))) settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
  }
  ApplyStationheadResourceBlocking(environment_.Get(), webview_.Get(), config_,
                                   resourceBlockingArmed_, resourceRequestedToken_);
  ComPtr<ICoreWebView2_19> v19;
  if (config_.lowMemoryMode && SUCCEEDED(webview_.As(&v19))) {
    v19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
  }
  static const std::wstring startupScript =
      StationheadAutoplayScript(L"__homepanelPrimaryStationhead", L"stationhead");
  webview_->AddScriptToExecuteOnDocumentCreated(
      startupScript.c_str(),
      Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
          [this](HRESULT result, LPCWSTR) -> HRESULT {
            if (FAILED(result)) {
              log_.Warn(L"Primary Stationhead startup script registration failed " +
                        HResultHex(result));
            }
            if (!pendingAuthorizationUrl_.empty()) {
              const std::wstring authorizationUrl = pendingAuthorizationUrl_;
              pendingAuthorizationUrl_.clear();
              OpenSpotifyAuthorization(authorizationUrl);
            } else {
              NavigatePrimaryUrl(UnixMillis(), L"startup");
            }
            return S_OK;
          }).Get());

  webview_->add_NewWindowRequested(
      Callback<ICoreWebView2NewWindowRequestedEventHandler>(
          [this](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            if (!args) return S_OK;
            // Always mark the request handled so a failure below never falls through to
            // WebView2's default behavior of opening an uncontrolled top-level popup window.
            args->put_Handled(TRUE);
            if (!environment_ || !EnsureAuthHostWindow()) return S_OK;
            LPWSTR uriRaw = nullptr;
            args->get_Uri(&uriRaw);
            const std::wstring uri = uriRaw ? uriRaw : L"";
            if (uriRaw) CoTaskMemFree(uriRaw);
            ComPtr<ICoreWebView2Deferral> deferral;
            if (FAILED(args->GetDeferral(&deferral)) || !deferral) return S_OK;
            ComPtr<ICoreWebView2NewWindowRequestedEventArgs> popupArgs = args;
            CloseAuthWebView();
            spotifyAuthorization_ = true;
            selectedTab_ = StationheadTabKind::Auth;
            viewVisible_ = true;
            LayoutControllers();
            const HRESULT createResult = environment_->CreateCoreWebView2Controller(
                authHostWindow_, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                     [this, popupArgs, deferral, uri](HRESULT result,
                                         ICoreWebView2Controller* controller) -> HRESULT {
                                       if (FAILED(result) || !controller || shuttingDown_) {
                                         if (controller) controller->Close();
                                         spotifyAuthorization_ = false;
                                         deferral->Complete();
                                         PostChange();
                                         return S_OK;
                                       }
                                       authController_ = controller;
                                       authController_->put_IsVisible(FALSE);
                                       authController_->get_CoreWebView2(&authWebview_);
                                       ConfigureAuthWebView();
                                       if (SUCCEEDED(popupArgs->put_NewWindow(authWebview_.Get()))) {
                                         popupArgs->put_Handled(TRUE);
                                         SelectTab(StationheadTabKind::Auth);
                                         log_.Info(L"Stationhead popup attached to auth tab: " + uri);
                                       }
                                       deferral->Complete();
                                       PostChange();
                                       return S_OK;
                                     }).Get());
            if (FAILED(createResult)) {
              spotifyAuthorization_ = false;
              deferral->Complete();
              PostChange();
            }
            return S_OK;
          }).Get(), &newWindowToken_);

  webview_->add_WebMessageReceived(
      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            if (!args) return S_OK;
            LPWSTR rawText = nullptr;
            if (SUCCEEDED(args->TryGetWebMessageAsString(&rawText)) && rawText) {
              const std::wstring message(rawText);
              CoTaskMemFree(rawText);
              const int64_t now = UnixMillis();
              if (message == L"stationhead-playing") {
                audioPlaying_ = true;
                resourceBlockingArmed_ = true;
                noAudioSinceAt_ = 0;
                loginSessionActive_ = false;
                {
                  std::lock_guard lock(mutex_);
                  status_.audioPlaying = true;
                  status_.playing = true;
                  status_.loginRequired = false;
                  status_.navigating = false;
                  status_.detail = usedFallback_ ? L"fallback audio detected" : L"audio detected";
                }
                // Re-assert the z-order exactly once per confirmed playback
                // (each scheduled reload), instead of every tick. The startup
                // split preview stays up until App hands over to the dashboard.
                if (!startupPreviewActive_) SetVisible(false);
                PostChange(StationheadChangeReturnMain);
                return S_OK;
              }
              if (message == L"stationhead-stopped") {
                audioPlaying_ = false;
                if (!usedFallback_ && noAudioSinceAt_ == 0) noAudioSinceAt_ = now;
                {
                  std::lock_guard lock(mutex_);
                  status_.audioPlaying = false;
                  status_.playing = false;
                  status_.detail = usedFallback_
                      ? L"fallback audio stopped"
                      : L"primary audio stopped; waiting before fallback";
                }
                nextTickAt_ = 0;
                PostChange();
                return S_OK;
              }
              if (message == L"stationhead-login-required") {
                loginSessionActive_ = true;
                ShowForLogin();
                {
                  std::lock_guard lock(mutex_);
                  status_.loginRequired = true;
                  status_.detail = L"login required";
                }
                nextTickAt_ = 0;
                PostChange();
                return S_OK;
              }
            }

            LPWSTR messageRaw = nullptr;
            if (FAILED(args->get_WebMessageAsJson(&messageRaw)) || !messageRaw) return S_OK;
            const std::wstring messageJson = messageRaw;
            CoTaskMemFree(messageRaw);
            try {
              const auto message = winrt::Windows::Data::Json::JsonObject::Parse(messageJson);
              const std::wstring type = message.GetNamedString(L"type", L"").c_str();
              if (!spotifyAuthorization_ || (type != L"spotify-connected" && type != L"spotify-error")) {
                return S_OK;
              }
              spotifyAuthorization_ = false;
              {
                std::lock_guard lock(mutex_);
                status_.navigating = false;
                status_.spotifyConfigured = type == L"spotify-connected";
                status_.detail = type == L"spotify-connected"
                    ? L"Spotify authentication completed"
                    : L"Spotify authentication failed or cancelled";
              }
              SetVisible(false);
              PostChange(StationheadChangeReturnMain | StationheadChangeReleaseAuth);
            } catch (...) {
            }
            return S_OK;
          }).Get(), &webMessageToken_);

  webview_->add_NavigationCompleted(
      Callback<ICoreWebView2NavigationCompletedEventHandler>(
          [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            BOOL success = FALSE;
            COREWEBVIEW2_WEB_ERROR_STATUS webError{};
            if (args) {
              args->get_IsSuccess(&success);
              args->get_WebErrorStatus(&webError);
            }
            if (spotifyAuthorization_) {
              std::lock_guard lock(mutex_);
              status_.navigating = false;
              status_.detail = success ? L"Spotify login ready" : L"Spotify authentication navigation failed";
              PostChange();
              return S_OK;
            }
            const int64_t now = UnixMillis();
            {
              std::lock_guard lock(mutex_);
              status_.navigating = false;
              status_.detail = success ? L"station loaded" : L"station navigation failed";
            }
            if (success) {
              lastReloadAt_ = now;
              if (!usedFallback_ && !audioPlaying_.load(std::memory_order_relaxed) && noAudioSinceAt_ == 0) {
                noAudioSinceAt_ = now;
              }
              // A navigation replaced the document, so the injected volume
              // script state is gone; force ApplyVolume to re-run it.
              appliedVolumePercent_.store(-1, std::memory_order_relaxed);
              ApplyMute();
            } else {
              ScheduleRecreate(L"navigation failed " + std::to_wstring(static_cast<int>(webError)));
            }
            PostChange();
            return S_OK;
          }).Get(), &navigationToken_);

  webview_->add_ProcessFailed(
      Callback<ICoreWebView2ProcessFailedEventHandler>(
          [this](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
            COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
            if (args) args->get_ProcessFailedKind(&kind);
            {
              std::lock_guard lock(mutex_);
              status_.processFailed = true;
              status_.detail = L"ProcessFailed " + std::to_wstring(kind);
            }
            ScheduleRecreate(L"ProcessFailed");
            return S_OK;
          }).Get(), &processFailedToken_);

  {
    std::lock_guard lock(mutex_);
    const bool spotifyConfigured = status_.spotifyConfigured;
    status_ = {};
    status_.created = true;
    status_.navigating = true;
    status_.url = config_.url;
    status_.detail = L"起動中";
    status_.spotifyConfigured = spotifyConfigured;
  }
  createdAt_ = lastReloadAt_ = UnixMillis();
  noAudioSinceAt_ = createdAt_;
  usedFallback_ = false;
  resourceBlockingArmed_ = false;
}

void StationheadPlayer::ConfigureAuthWebView() {
  if (!authController_ || !authWebview_) return;
  appliedMuted_.store(-1, std::memory_order_relaxed);
  appliedVolumePercent_.store(-1, std::memory_order_relaxed);
  ComPtr<ICoreWebView2Controller2> controller2;
  if (SUCCEEDED(authController_.As(&controller2))) {
    COREWEBVIEW2_COLOR background{255, 7, 17, 28};
    controller2->put_DefaultBackgroundColor(background);
  }
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
          [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            BOOL success = FALSE;
            if (args) args->get_IsSuccess(&success);
            if (success) {
              SelectTab(StationheadTabKind::Auth);
              authWebview_->PostWebMessageAsJson(L"{\"type\":\"auth-tab-ready\"}");
            }
            return S_OK;
          }).Get(), &authNavigationToken_);
  authWebview_->add_WindowCloseRequested(
      Callback<ICoreWebView2WindowCloseRequestedEventHandler>(
          [this](ICoreWebView2*, IUnknown*) -> HRESULT {
            spotifyAuthorization_ = false;
            authPendingUrl_.clear();
            SelectTab(StationheadTabKind::None);
            PostChange(StationheadChangeReturnMain | StationheadChangeReleaseAuth);
            return S_OK;
          }).Get(), &authCloseToken_);
  authWebview_->add_WebMessageReceived(
      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            LPWSTR messageRaw = nullptr;
            if (!args || FAILED(args->get_WebMessageAsJson(&messageRaw)) || !messageRaw) return S_OK;
            const std::wstring messageJson = messageRaw;
            CoTaskMemFree(messageRaw);
            try {
              const auto message = winrt::Windows::Data::Json::JsonObject::Parse(messageJson);
              const std::wstring type = message.GetNamedString(L"type", L"").c_str();
              if (type != L"spotify-connected" && type != L"spotify-error") return S_OK;
              spotifyAuthorization_ = false;
              authPendingUrl_.clear();
              {
                std::lock_guard lock(mutex_);
                status_.navigating = false;
                status_.spotifyConfigured = type == L"spotify-connected";
                status_.detail = type == L"spotify-connected"
                    ? L"Spotify authentication completed"
                    : L"Spotify authentication failed or cancelled";
              }
              SelectTab(StationheadTabKind::None);
              PostChange(StationheadChangeReturnMain | StationheadChangeReleaseAuth);
            } catch (...) {
            }
            return S_OK;
          }).Get(), &authMessageToken_);
  authWebview_->add_ProcessFailed(
      Callback<ICoreWebView2ProcessFailedEventHandler>(
          [this](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) -> HRESULT {
            SelectTab(StationheadTabKind::None);
            PostChange(StationheadChangeReturnMain);
            return S_OK;
          }).Get(), &authProcessFailedToken_);
  ApplyMute();
  if (!authPendingUrl_.empty()) authWebview_->Navigate(authPendingUrl_.c_str());
}

void StationheadPlayer::CloseWebView() {
  if (webview_) {
    if (navigationToken_.value) webview_->remove_NavigationCompleted(navigationToken_);
    if (newWindowToken_.value) webview_->remove_NewWindowRequested(newWindowToken_);
    if (webMessageToken_.value) webview_->remove_WebMessageReceived(webMessageToken_);
    if (processFailedToken_.value) webview_->remove_ProcessFailed(processFailedToken_);
    if (resourceRequestedToken_.value) webview_->remove_WebResourceRequested(resourceRequestedToken_);
  }
  navigationToken_ = {};
  newWindowToken_ = {};
  webMessageToken_ = {};
  processFailedToken_ = {};
  resourceRequestedToken_ = {};
  resourceBlockingArmed_ = false;
  if (controller_) controller_->Close();
  webview_.Reset();
  controller_.Reset();
  environment_.Reset();
  appliedMuted_.store(-1, std::memory_order_relaxed);
  appliedVolumePercent_.store(-1, std::memory_order_relaxed);
  if (hostWindow_ && IsWindow(hostWindow_)) ShowWindow(hostWindow_, SW_HIDE);
  spotifyAuthorization_ = false;
  loginSessionActive_ = false;
  noAudioSinceAt_ = 0;
  std::lock_guard lock(mutex_);
  status_.created = false;
}

void StationheadPlayer::CloseAuthWebView() {
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
  authPendingUrl_.clear();
  appliedMuted_.store(-1, std::memory_order_relaxed);
  appliedVolumePercent_.store(-1, std::memory_order_relaxed);
}

void StationheadPlayer::Tick(int64_t nowMs) {
  if (shuttingDown_) return;
  if (nowMs < nextTickAt_ && !recreating_.load(std::memory_order_relaxed)) return;
  nextTickAt_ = nowMs + 60'000;
  if (recreating_.exchange(false)) {
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
  if (spotifyAuthorization_ || loginSessionActive_) {
    nextTickAt_ = nowMs + 1'000;
    return;
  }

  const int64_t reloadInterval = StationheadReloadIntervalMs(config_.reloadIntervalMinutes);
  if (reloadInterval > 0 && lastReloadAt_ > 0 && nowMs - lastReloadAt_ >= reloadInterval) {
    NavigatePrimaryUrl(nowMs, L"scheduled reload reset");
    PostChange(StationheadChangeScheduledReload);
    nextTickAt_ = nowMs + 1'000;
    return;
  }

  if (!usedFallback_ && !audioPlaying_.load(std::memory_order_relaxed) && noAudioSinceAt_ > 0 &&
      nowMs - noAudioSinceAt_ >= kPrimaryNoAudioFallbackMs && !config_.fallbackUrl.empty()) {
    NavigateStationheadUrl(nowMs, config_.fallbackUrl,
                           L"primary had no audio for 360s; switching to fallback", true);
    PostChange();
    nextTickAt_ = nowMs + 1'000;
    return;
  }

  int64_t next = nowMs + 30 * 60'000;
  const auto consider = [&](int64_t deadline) {
    if (deadline <= nowMs) next = nowMs + 1'000;
    else next = std::min(next, deadline);
  };
  if (reloadInterval > 0 && lastReloadAt_ > 0) consider(lastReloadAt_ + reloadInterval);
  if (!usedFallback_ && !audioPlaying_.load(std::memory_order_relaxed) && noAudioSinceAt_ > 0) {
    consider(noAudioSinceAt_ + kPrimaryNoAudioFallbackMs);
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
  loginSessionActive_ = false;
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

void StationheadPlayer::ShowForLogin() {
  SelectTab(StationheadTabKind::Stationhead);
  log_.Warn(L"Stationhead login required; Stationhead window visible");
}

void StationheadPlayer::ShowAfterAudioStop() {
  if (!webview_) return;
  selectedTab_ = StationheadTabKind::Stationhead;
  viewVisible_ = true;
  noAudioSinceAt_ = UnixMillis();
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
  bool loginRequired = false;
  {
    std::lock_guard lock(mutex_);
    loginRequired = status_.loginRequired;
  }
  if (spotifyAuthorization_ || loginRequired) {
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

void StationheadPlayer::SetMuted(bool muted) noexcept {
  audioMuted_.store(muted, std::memory_order_relaxed);
  ApplyMute();
}

bool StationheadPlayer::Muted() const noexcept {
  return audioMuted_.load(std::memory_order_relaxed);
}

void StationheadPlayer::SetVolume(double volume) noexcept {
  audioVolume_.store(std::clamp(volume, 0.0, 1.0), std::memory_order_relaxed);
  ApplyVolume();
}

double StationheadPlayer::Volume() const noexcept {
  return audioVolume_.load(std::memory_order_relaxed);
}

void StationheadPlayer::ApplyMute() const noexcept {
  const int muted = audioMuted_.load(std::memory_order_relaxed) ? 1 : 0;
  if (appliedMuted_.exchange(muted, std::memory_order_relaxed) != muted) {
    const BOOL value = muted ? TRUE : FALSE;
    const auto apply = [value](const ComPtr<ICoreWebView2>& view) noexcept {
      if (!view) return;
      ComPtr<ICoreWebView2_8> audio;
      if (SUCCEEDED(view.As(&audio)) && audio) audio->put_IsMuted(value);
    };
    apply(webview_);
    apply(authWebview_);
  }
  ApplyVolume();
}

void StationheadPlayer::ApplyVolume() const noexcept {
  const int percent = std::clamp(
      static_cast<int>(audioVolume_.load(std::memory_order_relaxed) * 100.0 + 0.5), 0, 100);
  if (appliedVolumePercent_.exchange(percent, std::memory_order_relaxed) == percent) return;
  const auto apply = [percent](const ComPtr<ICoreWebView2>& view) noexcept {
    if (!view) return;
    const std::wstring script = StationheadVolumeScript(percent);
    view->ExecuteScript(script.c_str(), nullptr);
  };
  apply(webview_);
  apply(authWebview_);
}

void StationheadPlayer::SetBounds(const RECT& bounds) {
  if (EqualRect(&bounds_, &bounds)) return;
  bounds_ = bounds;
  if (startupPreviewActive_ || viewVisible_ || NeedsInteractiveWindow()) LayoutControllers();
  else KeepPlaybackBehindDashboard();
}

void StationheadPlayer::SelectTab(StationheadTabKind tab) {
  if (tab == StationheadTabKind::None && NeedsInteractiveWindow()) {
    tab = spotifyAuthorization_ ? StationheadTabKind::Auth : StationheadTabKind::Stationhead;
  }
  if (selectedTab_ == tab) {
    if (tab == StationheadTabKind::None && !viewVisible_) return;
    SetVisible(tab != StationheadTabKind::None);
    return;
  }
  selectedTab_ = tab;
  SetVisible(tab != StationheadTabKind::None);
}

bool StationheadPlayer::HasAuthTab() const {
  return authController_ != nullptr || !authPendingUrl_.empty();
}

HWND StationheadPlayer::ActiveHostWindowForAccountSetup() const noexcept {
  if (selectedTab_ == StationheadTabKind::Auth && authHostWindow_ && IsWindow(authHostWindow_)) {
    return authHostWindow_;
  }
  return hostWindow_;
}

bool StationheadPlayer::NeedsInteractiveWindow() const {
  return selectedTab_ == StationheadTabKind::Auth || spotifyAuthorization_ || loginSessionActive_;
}

void StationheadPlayer::ScheduleRecreate(const std::wstring& reason) {
  nextTickAt_ = 0;
  if (shuttingDown_ || recreating_.exchange(true)) return;
  {
    std::lock_guard lock(mutex_);
    status_.detail = L"recreate scheduled: " + reason;
  }
  log_.Warn(L"Stationhead WebView recreate scheduled: " + reason);
  PostChange();
}

}  // namespace hp
