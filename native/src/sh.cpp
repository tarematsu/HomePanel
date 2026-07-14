#include "sh.h"
#include "json_helpers.h"
#include "shared_webview_environment.h"
#include "sh_shared.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {
constexpr int64_t kPrimaryNoAudioFallbackMs = 360'000;
constexpr int64_t kPrimaryFallbackMonitorGraceMs = 60'000;
constexpr int64_t kDailyPlayStatsIntervalMs = 5 * 60'000;
constexpr int64_t kDailyPlayStatsRetryMs = 30'000;

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
  noAudioSinceAt_ = 0;
  fallbackMonitorAfterAt_ = 0;
  nextTickAt_ = 0;
}

void StationheadPlayer::ApplyAudioPlaybackState(bool playing, int64_t nowMs,
                                                const std::wstring& source) {
  const bool changed =
      audioPlaying_.exchange(playing, std::memory_order_relaxed) != playing;
  if (playing) {
    resourceBlockingArmed_ = true;
    noAudioSinceAt_ = 0;
    fallbackMonitorAfterAt_ = 0;
    loginSessionActive_ = false;
    {
      std::lock_guard lock(mutex_);
      status_.audioPlaying = true;
      status_.playing = true;
      status_.loginRequired = false;
      status_.navigating = false;
      status_.detail = (usedFallback_ ? L"fallback audio detected" : L"audio detected") +
                       (source.empty() ? L"" : L" (" + source + L")");
    }
    if (!startupPreviewActive_) SetVisible(false);
    if (changed) log_.Info(L"Stationhead audio playing (" + source + L")");
    PostChange(StationheadChangeReturnMain);
    return;
  }




  if (!usedFallback_ &&
      (changed || (fallbackMonitorAfterAt_ == 0 && noAudioSinceAt_ == 0))) {
    fallbackMonitorAfterAt_ = nowMs + kPrimaryFallbackMonitorGraceMs;
    noAudioSinceAt_ = 0;
  }
  {
    std::lock_guard lock(mutex_);
    status_.audioPlaying = false;
    status_.playing = false;
    status_.detail = usedFallback_
        ? L"fallback audio stopped"
        : L"primary audio stopped; waiting before fallback";
  }
  nextTickAt_ = 0;
  if (changed) log_.Warn(L"Stationhead audio stopped (" + source + L")");
  PostChange();
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
  if (!fallbackActive) {
    fallbackMonitorAfterAt_ = nowMs + kPrimaryFallbackMonitorGraceMs;
  }
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

void StationheadPlayer::PollDailyPlayStats(int64_t nowMs) {
  if (!webview_) return;
  lastDailyPlayStatsAt_ = nowMs;
  const std::wstring script = StationheadApiPlayStatsScript(config_.channelId);
  webview_->ExecuteScript(script.c_str(), nullptr);
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
        const HRESULT started = environment_->CreateCoreWebView2Controller(
            hostWindow_, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
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
  authCallbackAlive_->store(false, std::memory_order_release);
  authCallbackAlive_ = std::make_shared<std::atomic<bool>>(true);
  const auto alive = authCallbackAlive_;
  environment_->CreateCoreWebView2Controller(
      authHostWindow_, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
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
                           }).Get());
}

void StationheadPlayer::ConfigureWebView() {
  const auto alive = createCallbackAlive_;

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





  ComPtr<ICoreWebView2_8> audioView;
  if (SUCCEEDED(webview_.As(&audioView)) && audioView) {
    const HRESULT audioHandlerResult = audioView->add_IsDocumentPlayingAudioChanged(
        Callback<ICoreWebView2IsDocumentPlayingAudioChangedEventHandler>(
            [this, alive](ICoreWebView2*, IUnknown*) -> HRESULT {
              if (!CallbackAlive(alive) || shuttingDown_ || !webview_) return S_OK;
              ComPtr<ICoreWebView2_8> currentAudioView;
              if (FAILED(webview_.As(&currentAudioView)) || !currentAudioView) return S_OK;
              BOOL playing = FALSE;
              if (SUCCEEDED(currentAudioView->get_IsDocumentPlayingAudio(&playing))) {
                ApplyAudioPlaybackState(playing != FALSE, UnixMillis(), L"WebView2");
              }
              return S_OK;
            }).Get(),
        &audioPlayingChangedToken_);
    if (SUCCEEDED(audioHandlerResult)) {
      nativeAudioTracking_ = true;
      BOOL playing = FALSE;
      if (SUCCEEDED(audioView->get_IsDocumentPlayingAudio(&playing))) {
        ApplyAudioPlaybackState(playing != FALSE, UnixMillis(), L"WebView2 initial");
      }
    } else {
      audioPlayingChangedToken_ = {};
      nativeAudioTracking_ = false;
      log_.Warn(L"WebView2 native audio tracking unavailable " + HResultHex(audioHandlerResult));
    }
  }

  ComPtr<ICoreWebView2_19> v19;
  if (config_.lowMemoryMode && SUCCEEDED(webview_.As(&v19))) {
    v19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
  }
  static const std::wstring authCaptureScript = StationheadAuthCaptureScript();
  webview_->AddScriptToExecuteOnDocumentCreated(authCaptureScript.c_str(), nullptr);
  static const std::wstring startupScript =
      StationheadAutoplayScript(L"__homepanelPrimaryStationhead", L"stationhead");
  webview_->AddScriptToExecuteOnDocumentCreated(
      startupScript.c_str(),
      Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
          [this, alive](HRESULT result, LPCWSTR) -> HRESULT {
            if (!CallbackAlive(alive)) return S_OK;
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
          [this, alive](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            if (!CallbackAlive(alive) || !args) return S_OK;


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
            authCallbackAlive_ = std::make_shared<std::atomic<bool>>(true);
            const auto authAlive = authCallbackAlive_;
            spotifyAuthorization_ = true;
            selectedTab_ = StationheadTabKind::Auth;
            viewVisible_ = true;
            LayoutControllers();
            const HRESULT createResult = environment_->CreateCoreWebView2Controller(
                authHostWindow_, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                     [this, popupArgs, deferral, uri, authAlive](HRESULT result,
                                         ICoreWebView2Controller* controller) -> HRESULT {
                                       if (!CallbackAlive(authAlive)) {
                                         if (controller) controller->Close();
                                         deferral->Complete();
                                         return S_OK;
                                       }
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
              authCallbackAlive_->store(false, std::memory_order_release);
              spotifyAuthorization_ = false;
              deferral->Complete();
              PostChange();
            }
            return S_OK;
          }).Get(), &newWindowToken_);

  webview_->add_WebMessageReceived(
      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [this, alive](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            if (!CallbackAlive(alive) || !args) return S_OK;
            LPWSTR rawText = nullptr;
            if (SUCCEEDED(args->TryGetWebMessageAsString(&rawText)) && rawText) {
              const std::wstring message(rawText);
              CoTaskMemFree(rawText);
              const int64_t now = UnixMillis();
              if (message == L"stationhead-playing") {
                if (!nativeAudioTracking_) {
                  ApplyAudioPlaybackState(true, now, L"page heuristic");
                }
                return S_OK;
              }
              if (message == L"stationhead-stopped") {
                if (!nativeAudioTracking_) {
                  ApplyAudioPlaybackState(false, now, L"page heuristic");
                }
                return S_OK;
              }
              if (message == L"stationhead-start-attempted") {



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
              if (type == L"stationhead-play-stats") {
                using winrt::Windows::Data::Json::JsonValueType;
                const auto data = json::Object(message, L"data");
                const auto chart = json::Array(data, L"chart_data");
                std::vector<StationheadDailyPlayPoint> points;
                for (uint32_t index = 0; index < chart.Size() && points.size() < 40; ++index) {
                  if (chart.GetAt(index).ValueType() != JsonValueType::Object) continue;
                  const auto point = chart.GetObjectAt(index);
                  if (!point.HasKey(L"ts") || !point.HasKey(L"val")) continue;
                  points.push_back({
                      static_cast<int64_t>(json::Number(point, L"ts", 0)),
                      static_cast<int>(json::Number(point, L"val", 0)),
                  });
                }
                if (points.empty()) {
                  log_.Warn(L"Stationhead authenticated stats response contained no valid chart points");
                  return S_OK;
                }
                {
                  std::lock_guard lock(mutex_);
                  status_.dailyPlayCounts = std::move(points);
                  status_.dailyPlayStatsUpdatedAt = UnixMillis();
                  status_.detail = L"authenticated Stationhead API stats updated";
                }
                PostChange();
                return S_OK;
              }
              if (type == L"stationhead-play-stats-auth-failed") {
                const int status = static_cast<int>(json::Number(message, L"status", 0));
                log_.Warn(L"Stationhead authenticated stats rejected with HTTP " +
                          std::to_wstring(status) + L"; waiting for the page session to refresh");
                const int64_t now = UnixMillis();
                lastDailyPlayStatsAt_ = now - kDailyPlayStatsIntervalMs + kDailyPlayStatsRetryMs;
                nextTickAt_ = now + kDailyPlayStatsRetryMs;
                return S_OK;
              }
              if (type == L"stationhead-auth-ready") {
                lastDailyPlayStatsAt_ = 0;
                nextTickAt_ = 0;
                return S_OK;
              }
              if (type == L"stationhead-play-stats-error") {
                const std::wstring error = message.GetNamedString(L"error", L"unknown").c_str();
                log_.Warn(L"Stationhead authenticated stats unavailable: " + error);
                if (error == L"no-auth-header") {
                  const int64_t now = UnixMillis();
                  lastDailyPlayStatsAt_ = now - kDailyPlayStatsIntervalMs + kDailyPlayStatsRetryMs;
                  nextTickAt_ = now + kDailyPlayStatsRetryMs;
                }
                return S_OK;
              }
              if (!spotifyAuthorization_ ||
                  (type != L"spotify-connected" && type != L"spotify-error")) {
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
              log_.Warn(L"Stationhead authenticated API stats message had an invalid response shape");
            }
            return S_OK;
          }).Get(), &webMessageToken_);

  webview_->add_NavigationCompleted(
      Callback<ICoreWebView2NavigationCompletedEventHandler>(
          [this, alive](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            if (!CallbackAlive(alive)) return S_OK;
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
          [this, alive](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
            if (!CallbackAlive(alive)) return S_OK;
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
  noAudioSinceAt_ = 0;
  usedFallback_ = false;
  resourceBlockingArmed_ = false;
}

void StationheadPlayer::ConfigureAuthWebView() {
  if (!authController_ || !authWebview_) return;
  const auto alive = authCallbackAlive_;
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
          [this, alive](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            if (!CallbackAlive(alive)) return S_OK;
            BOOL success = FALSE;
            if (args) args->get_IsSuccess(&success);
            if (success) {
              SelectTab(StationheadTabKind::Auth);
              if (authWebview_) authWebview_->PostWebMessageAsJson(L"{\"type\":\"auth-tab-ready\"}");
            }
            return S_OK;
          }).Get(), &authNavigationToken_);
  authWebview_->add_WindowCloseRequested(
      Callback<ICoreWebView2WindowCloseRequestedEventHandler>(
          [this, alive](ICoreWebView2*, IUnknown*) -> HRESULT {
            if (!CallbackAlive(alive)) return S_OK;
            spotifyAuthorization_ = false;
            authPendingUrl_.clear();
            SelectTab(StationheadTabKind::None);
            PostChange(StationheadChangeReleaseAuth);
            return S_OK;
          }).Get(), &authCloseToken_);
  authWebview_->add_WebMessageReceived(
      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [this, alive](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            if (!CallbackAlive(alive)) return S_OK;
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
              PostChange(StationheadChangeReleaseAuth);
            } catch (...) {
            }
            return S_OK;
          }).Get(), &authMessageToken_);
  authWebview_->add_ProcessFailed(
      Callback<ICoreWebView2ProcessFailedEventHandler>(
          [this, alive](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) -> HRESULT {
            if (!CallbackAlive(alive)) return S_OK;
            SelectTab(StationheadTabKind::None);
            PostChange();
            return S_OK;
          }).Get(), &authProcessFailedToken_);
  ApplyMute();
  if (!authPendingUrl_.empty()) authWebview_->Navigate(authPendingUrl_.c_str());
}

void StationheadPlayer::CloseWebView() {
  createCallbackAlive_->store(false, std::memory_order_release);
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
    if (webMessageToken_.value) webview_->remove_WebMessageReceived(webMessageToken_);
    if (processFailedToken_.value) webview_->remove_ProcessFailed(processFailedToken_);
    if (resourceRequestedToken_.value) webview_->remove_WebResourceRequested(resourceRequestedToken_);
  }
  navigationToken_ = {};
  newWindowToken_ = {};
  webMessageToken_ = {};
  processFailedToken_ = {};
  resourceRequestedToken_ = {};
  audioPlayingChangedToken_ = {};
  nativeAudioTracking_ = false;
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
  fallbackMonitorAfterAt_ = 0;
  std::lock_guard lock(mutex_);
  status_.created = false;
}

void StationheadPlayer::CloseAuthWebView() {
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

  if (nowMs - lastDailyPlayStatsAt_ >= kDailyPlayStatsIntervalMs) {
    PollDailyPlayStats(nowMs);
  }

  const bool silentPrimary =
      !usedFallback_ && !audioPlaying_.load(std::memory_order_relaxed);
  if (silentPrimary && fallbackMonitorAfterAt_ > 0 &&
      nowMs >= fallbackMonitorAfterAt_ && noAudioSinceAt_ == 0) {
    noAudioSinceAt_ = nowMs;
  }
  // Silence recovery is safety-critical and must run before the coordinated
  // maintenance reload. Otherwise a silent secondary window can keep returning
  // from the reload-wait branch forever and prevent primary fallback.
  if (silentPrimary && noAudioSinceAt_ > 0 &&
      nowMs - noAudioSinceAt_ >= kPrimaryNoAudioFallbackMs &&
      !config_.fallbackUrl.empty()) {
    log_.Warn(L"Stationhead primary had no " +
              std::wstring(nativeAudioTracking_ ? L"WebView2 audio" : L"detected audio") +
              L" for 360s; switching to fallback");
    NavigateStationheadUrl(nowMs, config_.fallbackUrl,
                           L"primary had no audio for 360s; switching to fallback", true);
    PostChange();
    nextTickAt_ = nowMs + 1'000;
    return;
  }

  const int64_t reloadInterval = StationheadReloadIntervalMs(config_.reloadIntervalMinutes);
  if (reloadInterval > 0 && lastReloadAt_ > 0 && nowMs - lastReloadAt_ >= reloadInterval) {
    const bool secondaryConfigured = config_.secondaryEnabled && !config_.secondaryUrl.empty();
    if (secondaryConfigured &&
        (!window_ || !IsWindow(window_) ||
         SendMessageW(window_, WM_HP_PRIMARY_RELOAD_READY, 0, 0) == 0)) {
      {
        std::lock_guard lock(mutex_);
        status_.detail = L"maintenance reload waiting for secondary audio";
      }
      nextTickAt_ = nowMs + 30'000;
      return;
    }
    NavigatePrimaryUrl(nowMs, L"50-minute scheduled reload reset");
    nextTickAt_ = nowMs + 1'000;
    return;
  }

  int64_t next = nowMs + 30 * 60'000;
  const auto consider = [&](int64_t deadline) {
    if (deadline <= nowMs) next = nowMs + 1'000;
    else next = std::min(next, deadline);
  };
  if (reloadInterval > 0 && lastReloadAt_ > 0) consider(lastReloadAt_ + reloadInterval);
  consider(lastDailyPlayStatsAt_ + kDailyPlayStatsIntervalMs);
  if (silentPrimary && fallbackMonitorAfterAt_ > nowMs) {
    consider(fallbackMonitorAfterAt_);
  }
  if (silentPrimary && noAudioSinceAt_ > 0) {
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
  const int64_t now = UnixMillis();
  fallbackMonitorAfterAt_ = now;
  noAudioSinceAt_ = now;
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
  return selectedTab_ == StationheadTabKind::Auth ||
         spotifyAuthorization_ ||
         loginSessionActive_ ||
         !audioPlaying_.load(std::memory_order_relaxed);
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
