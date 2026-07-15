#include "sh.h"
#include "json_helpers.h"
#include "sh_shared.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {
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
                ApplyAudioPlaybackState(playing != FALSE, L"WebView2");
              }
              return S_OK;
            }).Get(),
        &audioPlayingChangedToken_);
    if (SUCCEEDED(audioHandlerResult)) {
      nativeAudioTracking_ = true;
      BOOL playing = FALSE;
      if (SUCCEEDED(audioView->get_IsDocumentPlayingAudio(&playing))) {
        ApplyAudioPlaybackState(playing != FALSE, L"WebView2 initial");
      }
    } else {
      audioPlayingChangedToken_ = {};
      nativeAudioTracking_ = false;
      log_.Warn(L"Stationhead " + std::wstring(RoleTag()) + L" WebView2 native audio tracking unavailable " + HResultHex(audioHandlerResult));
    }
  }

  ComPtr<ICoreWebView2_19> v19;
  if (config_.lowMemoryMode && SUCCEEDED(webview_.As(&v19))) {
    v19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
  }
  ApplyMute();
  if (IsSecondary()) EnsureDistinctBrowserIdentity();

  static const std::wstring authCaptureScript = StationheadAuthCaptureScript();
  webview_->AddScriptToExecuteOnDocumentCreated(authCaptureScript.c_str(), nullptr);
  static const std::wstring primaryStartupScript =
      StationheadAutoplayScript(L"__homepanelPrimaryStationhead", L"stationhead");
  static const std::wstring secondaryStartupScript =
      StationheadAutoplayScript(L"__homepanelSecondaryStationhead", L"secondary");
  const std::wstring& startupScript = IsSecondary() ? secondaryStartupScript : primaryStartupScript;
  webview_->AddScriptToExecuteOnDocumentCreated(
      startupScript.c_str(),
      Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
          [this, alive](HRESULT result, LPCWSTR) -> HRESULT {
            if (!CallbackAlive(alive)) return S_OK;
            if (FAILED(result)) {
              log_.Warn(L"Stationhead " + std::wstring(RoleTag()) + L" startup script registration failed " +
                        HResultHex(result));
            }
            if (!pendingAuthorizationUrl_.empty()) {
              const std::wstring authorizationUrl = pendingAuthorizationUrl_;
              pendingAuthorizationUrl_.clear();
              OpenSpotifyAuthorization(authorizationUrl);
            } else {
              NavigateCurrentUrl(UnixMillis(), L"startup");
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
            SelectTab(StationheadTabKind::Auth);

            const auto onController = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [this, popupArgs, deferral, uri, authAlive](HRESULT result,
                    ICoreWebView2Controller* controller) -> HRESULT {
                  if (!CallbackAlive(authAlive)) {
                    if (controller) controller->Close();
                    deferral->Complete();
                    return S_OK;
                  }
                  if (FAILED(result) || !controller || shuttingDown_) {
                    if (controller) controller->Close();
                    FinishSpotifyAuthorization(L"Spotify popup creation failed " + HResultHex(result));
                    deferral->Complete();
                    return S_OK;
                  }
                  authController_ = controller;
                  authController_->put_IsVisible(FALSE);
                  authController_->get_CoreWebView2(&authWebview_);
                  if (!authWebview_) {
                    FinishSpotifyAuthorization(L"Spotify popup WebView unavailable");
                    deferral->Complete();
                    return S_OK;
                  }
                  ConfigureAuthWebView();
                  if (SUCCEEDED(popupArgs->put_NewWindow(authWebview_.Get()))) {
                    popupArgs->put_Handled(TRUE);
                    SelectTab(StationheadTabKind::Auth);
                    log_.Info(L"Stationhead " + std::wstring(RoleTag()) + L" popup attached to auth tab: " + uri);
                  } else {
                    FinishSpotifyAuthorization(L"Spotify popup attachment failed");
                  }
                  deferral->Complete();
                  return S_OK;
                });

            HRESULT createResult = E_FAIL;
            if (!IsSecondary()) {
              createResult = environment_->CreateCoreWebView2Controller(authHostWindow_, onController.Get());
            } else {
              ComPtr<ICoreWebView2Environment10> environment10;
              if (FAILED(environment_.As(&environment10)) || !environment10) {
                FinishSpotifyAuthorization(L"WebView2 multi-profile API unavailable");
                deferral->Complete();
                return S_OK;
              }
              ComPtr<ICoreWebView2ControllerOptions> options;
              if (FAILED(environment10->CreateCoreWebView2ControllerOptions(options.GetAddressOf())) || !options) {
                FinishSpotifyAuthorization(L"Spotify popup profile options creation failed");
                deferral->Complete();
                return S_OK;
              }
              options->put_ProfileName(L"stationhead-secondary");
              options->put_IsInPrivateModeEnabled(FALSE);
              createResult = environment10->CreateCoreWebView2ControllerWithOptions(
                  authHostWindow_, options.Get(), onController.Get());
            }
            if (FAILED(createResult)) {
              authCallbackAlive_->store(false, std::memory_order_release);
              FinishSpotifyAuthorization(L"Spotify popup creation could not start " + HResultHex(createResult));
              deferral->Complete();
            }
            return S_OK;
          }).Get(), &newWindowToken_);

  webview_->add_WebMessageReceived(
      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [this, alive](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            if (!CallbackAlive(alive) || !args) return S_OK;
            const std::wstring prefix = IsSecondary() ? L"secondary" : L"stationhead";
            LPWSTR rawText = nullptr;
            if (SUCCEEDED(args->TryGetWebMessageAsString(&rawText)) && rawText) {
              const std::wstring message(rawText);
              CoTaskMemFree(rawText);
              if (message == prefix + L"-playing") {
                if (!nativeAudioTracking_) ApplyAudioPlaybackState(true, L"page heuristic");
                return S_OK;
              }
              if (message == prefix + L"-stopped") {
                if (!nativeAudioTracking_) ApplyAudioPlaybackState(false, L"page heuristic");
                return S_OK;
              }
              if (message == prefix + L"-start-visible") {
                AttemptNativeStartClick(UnixMillis());
                return S_OK;
              }
              if (message == prefix + L"-login-required") {
                loginRequired_ = true;
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
                lastDailyPlayStatsAt_ = now;
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
                  lastDailyPlayStatsAt_ = now;
                  nextTickAt_ = now + kDailyPlayStatsRetryMs;
                }
                return S_OK;
              }
              if (type == L"stationhead-auth-probe") {
                authProbeInFlight_ = false;
                authProbeStartedAt_ = 0;
                const std::wstring state = message.GetNamedString(L"state", L"").c_str();
                if (state == L"auth-failed") {
                  loginRequired_ = true;
                  ShowForLogin();
                  {
                    std::lock_guard lock(mutex_);
                    status_.loginRequired = true;
                    status_.detail = L"secondary Stationhead authentication expired";
                  }
                  log_.Warn(L"Secondary Stationhead authentication probe rejected with HTTP " +
                            std::to_wstring(static_cast<int>(message.GetNamedNumber(L"status", 0))));
                  PostChange();
                } else if (state == L"ok") {
                  std::lock_guard lock(mutex_);
                  status_.detail = L"secondary Stationhead authentication probe ok";
                } else if (state == L"no-auth-header") {
                  std::lock_guard lock(mutex_);
                  status_.detail = L"secondary Stationhead auth probe waiting for session";
                } else {
                  log_.Warn(L"Secondary Stationhead auth probe returned an error");
                }
                return S_OK;
              }
              if (!spotifyAuthorization_ ||
                  (type != L"spotify-connected" && type != L"spotify-error")) {
                return S_OK;
              }
              {
                std::lock_guard lock(mutex_);
                status_.navigating = false;
                status_.spotifyConfigured = type == L"spotify-connected";
              }
              FinishSpotifyAuthorization(type == L"spotify-connected"
                  ? L"Spotify authentication completed"
                  : L"Spotify authentication failed or cancelled");
            } catch (...) {
              log_.Warn(L"Stationhead API message had an invalid response shape");
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
              ScheduleRecreate(L"navigation failed " + std::to_wstring(static_cast<int>(webError)), 5'000);
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
            ScheduleRecreate(L"ProcessFailed", 5'000);
            return S_OK;
          }).Get(), &processFailedToken_);

  {
    std::lock_guard lock(mutex_);
    const bool spotifyConfigured = status_.spotifyConfigured;
    status_ = {};
    status_.created = true;
    status_.navigating = true;
    status_.url = CurrentStationheadUrl();
    status_.detail = IsSecondary() ? L"creating isolated WebView2 profile" : L"起動中";
    status_.spotifyConfigured = spotifyConfigured;
  }
  createdAt_ = lastReloadAt_ = UnixMillis();
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
  ComPtr<ICoreWebView2_19> authV19;
  if (config_.lowMemoryMode && SUCCEEDED(authWebview_.As(&authV19)) && authV19) {
    authV19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
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
            FinishSpotifyAuthorization(L"Spotify login closed; returning to Stationhead");
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
              {
                std::lock_guard lock(mutex_);
                status_.navigating = false;
                status_.spotifyConfigured = type == L"spotify-connected";
              }
              FinishSpotifyAuthorization(type == L"spotify-connected"
                  ? L"Spotify authentication completed"
                  : L"Spotify authentication failed or cancelled");
            } catch (...) {
            }
            return S_OK;
          }).Get(), &authMessageToken_);
  authWebview_->add_ProcessFailed(
      Callback<ICoreWebView2ProcessFailedEventHandler>(
          [this, alive](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) -> HRESULT {
            if (!CallbackAlive(alive)) return S_OK;
            FinishSpotifyAuthorization(L"Spotify login WebView failed");
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
  identityWebview_ = nullptr;
  if (controller_) controller_->Close();
  webview_.Reset();
  controller_.Reset();
  environment_.Reset();
  appliedMuted_.store(-1, std::memory_order_relaxed);
  appliedVolumePercent_.store(-1, std::memory_order_relaxed);
  if (hostWindow_ && IsWindow(hostWindow_)) ShowWindow(hostWindow_, SW_HIDE);
  spotifyAuthorization_ = false;
  loginRequired_ = false;
  lastAuthProbeAt_ = 0;
  authProbeStartedAt_ = 0;
  authProbeInFlight_ = false;
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
  if (authHostWindow_ && IsWindow(authHostWindow_)) ShowWindow(authHostWindow_, SW_HIDE);
}

}  // namespace hp
