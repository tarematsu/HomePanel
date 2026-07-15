




#include "secondary_sh.h"
#include "sh_shared.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {

void SecondaryStationheadPlayer::NavigateInitialStationhead(
    const std::shared_ptr<std::atomic<bool>>& alive) {
  if (!CallbackAlive(alive) || shuttingDown_ || !webview_) return;
  SetStartupBounds();
  lastReloadAt_ = UnixMillis();
  const std::wstring url = CurrentStationheadUrl();
  {
    std::lock_guard lock(mutex_);
    status_.created = true;
    status_.navigating = true;
    status_.processFailed = false;
    status_.url = url;
    status_.detail = L"loading secondary station";
  }
  const HRESULT result = webview_->Navigate(url.c_str());
  if (FAILED(result)) {
    ScheduleRetry(L"initial navigation failed " + HResultHex(result), 1'000);
    return;
  }
  log_.Info(L"Secondary Stationhead started with isolated profile: " + url);
}

void SecondaryStationheadPlayer::ConfigureWebView() {

  appliedMuted_.store(-1, std::memory_order_relaxed);
  appliedVolumePercent_.store(-1, std::memory_order_relaxed);
  const auto alive = callbackAlive_;
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
  ApplyStationheadResourceBlocking(environment_.Get(), webview_.Get(), config_, resourceBlockingArmed_, resourceRequestedToken_);




  ComPtr<ICoreWebView2_8> audioView;
  if (SUCCEEDED(webview_.As(&audioView)) && audioView) {
    const HRESULT audioResult = audioView->add_IsDocumentPlayingAudioChanged(
        Callback<ICoreWebView2IsDocumentPlayingAudioChangedEventHandler>(
            [this, alive](ICoreWebView2*, IUnknown*) -> HRESULT {
              if (!CallbackAlive(alive) || shuttingDown_ || !webview_) return S_OK;
              ComPtr<ICoreWebView2_8> currentAudioView;
              if (FAILED(webview_.As(&currentAudioView)) || !currentAudioView) return S_OK;
              BOOL playing = FALSE;
              if (SUCCEEDED(currentAudioView->get_IsDocumentPlayingAudio(&playing))) {
                ApplyPlaybackState(playing != FALSE, L"WebView2");
              }
              return S_OK;
            }).Get(),
        &audioPlayingChangedToken_);
    if (SUCCEEDED(audioResult)) {
      nativeAudioTracking_ = true;
      BOOL playing = FALSE;
      if (SUCCEEDED(audioView->get_IsDocumentPlayingAudio(&playing))) {
        ApplyPlaybackState(playing != FALSE, L"WebView2 initial");
      }
    } else {
      audioPlayingChangedToken_ = {};
      nativeAudioTracking_ = false;
      log_.Warn(L"Secondary WebView2 native audio tracking unavailable " + HResultHex(audioResult));
    }
  }

  ComPtr<ICoreWebView2_19> v19;
  if (config_.lowMemoryMode && SUCCEEDED(webview_.As(&v19))) {
    v19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
  }
  ApplyAudioState();
  ComPtr<ICoreWebView2Controller2> controller2;
  if (SUCCEEDED(controller_.As(&controller2))) {
    COREWEBVIEW2_COLOR background{255, 0, 0, 0};
    controller2->put_DefaultBackgroundColor(background);
  }
  static const std::wstring startupScript =
      StationheadAutoplayScript(L"__homepanelSecondaryStationhead", L"secondary");
  static const std::wstring authCaptureScript = StationheadAuthCaptureScript();
  const HRESULT authCaptureResult = webview_->AddScriptToExecuteOnDocumentCreated(
      authCaptureScript.c_str(),
      Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
          [this, alive](HRESULT result, LPCWSTR) -> HRESULT {
            if (!CallbackAlive(alive) || shuttingDown_) return S_OK;
            if (FAILED(result)) {
              log_.Warn(L"Secondary Stationhead auth capture script registration failed " +
                        HResultHex(result));
              ScheduleRetry(L"auth capture script registration failed");
              return S_OK;
            }
            if (!webview_) return S_OK;
            const HRESULT startupResult = webview_->AddScriptToExecuteOnDocumentCreated(
                startupScript.c_str(),
                Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
                    [this, alive](HRESULT startupStatus, LPCWSTR) -> HRESULT {
                      if (!CallbackAlive(alive) || shuttingDown_) return S_OK;
                      if (FAILED(startupStatus)) {
                        log_.Warn(L"Secondary Stationhead startup script registration failed " +
                                  HResultHex(startupStatus));
                        ScheduleRetry(L"startup script registration failed");
                        return S_OK;
                      }
                      NavigateInitialStationhead(alive);
                      return S_OK;
                    }).Get());
            if (FAILED(startupResult)) {
              log_.Warn(L"Secondary Stationhead startup script registration could not start " +
                        HResultHex(startupResult));
              ScheduleRetry(L"startup script registration could not start");
            }
            return S_OK;
          }).Get());
  if (FAILED(authCaptureResult)) {
    log_.Warn(L"Secondary Stationhead auth capture script registration could not start " +
              HResultHex(authCaptureResult));
    ScheduleRetry(L"auth capture script registration could not start");
  }
  webview_->add_NewWindowRequested(
      Callback<ICoreWebView2NewWindowRequestedEventHandler>(
          [this, alive](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            if (!args) return S_OK;


            args->put_Handled(TRUE);
            if (!CallbackAlive(alive) || shuttingDown_ || !environment_) return S_OK;
            LPWSTR uriRaw = nullptr;
            args->get_Uri(&uriRaw);
            const std::wstring uri = uriRaw ? uriRaw : L"";
            if (uriRaw) CoTaskMemFree(uriRaw);
            if (uri.empty() || !EnsureAuthHostWindow()) return S_OK;
            ComPtr<ICoreWebView2Environment10> environment10;
            if (FAILED(environment_.As(&environment10)) || !environment10) return S_OK;
            ComPtr<ICoreWebView2ControllerOptions> options;
            if (FAILED(environment10->CreateCoreWebView2ControllerOptions(options.GetAddressOf())) || !options) return S_OK;
            options->put_ProfileName(kProfileName);
            options->put_IsInPrivateModeEnabled(FALSE);
            ComPtr<ICoreWebView2Deferral> deferral;
            if (FAILED(args->GetDeferral(&deferral)) || !deferral) return S_OK;
            ComPtr<ICoreWebView2NewWindowRequestedEventArgs> popupArgs = args;
            CloseAuthWebView();
            authCallbackAlive_ = std::make_shared<std::atomic<bool>>(true);
            const auto authAlive = authCallbackAlive_;
            spotifyAuthorization_ = true;
            authClosePending_ = false;
            SetStatus(L"Spotify login loading in secondary profile");
            ShowInteractive(true);
            const HRESULT started = environment10->CreateCoreWebView2ControllerWithOptions(
                authHostWindow_, options.Get(),
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [this, alive, authAlive, popupArgs, deferral, uri](HRESULT result,
                        ICoreWebView2Controller* controller) -> HRESULT {
                      if (!CallbackAlive(alive) || !CallbackAlive(authAlive) || shuttingDown_) {
                        if (controller) controller->Close();
                        deferral->Complete();
                        return S_OK;
                      }
                      if (FAILED(result) || !controller) {
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
                      const HRESULT attachResult = popupArgs->put_NewWindow(authWebview_.Get());
                      if (SUCCEEDED(attachResult)) {
                        popupArgs->put_Handled(TRUE);
                        ShowInteractive(true);
                        log_.Info(L"Secondary Stationhead Spotify popup attached: " + uri);
                      } else {
                        FinishSpotifyAuthorization(L"Spotify popup attachment failed " + HResultHex(attachResult));
                      }
                      deferral->Complete();
                      return S_OK;
                    }).Get());
            if (FAILED(started)) {
              authAlive->store(false, std::memory_order_release);
              FinishSpotifyAuthorization(L"Spotify popup creation could not start " + HResultHex(started));
              deferral->Complete();
            }
            return S_OK;
          }).Get(), &newWindowToken_);
  webview_->add_WebMessageReceived(
      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [this, alive](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            if (!CallbackAlive(alive) || shuttingDown_) return S_OK;
            LPWSTR raw = nullptr;
            if (!args || FAILED(args->TryGetWebMessageAsString(&raw)) || !raw) return S_OK;
            const std::wstring message(raw);
            CoTaskMemFree(raw);
            if (message == L"secondary-playing") {
              if (!nativeAudioTracking_) ApplyPlaybackState(true, L"page heuristic");
            } else if (message == L"secondary-stopped") {
              if (!nativeAudioTracking_) ApplyPlaybackState(false, L"page heuristic");
            } else if (message == L"secondary-login-required") {
              loginRequired_ = true;
              ShowInteractive(true);
              SetStatus(L"login required in secondary profile");
            }
            return S_OK;
          }).Get(), &messageToken_);
  webview_->add_WebMessageReceived(
      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [this, alive](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            if (!CallbackAlive(alive) || shuttingDown_ || !args) return S_OK;
            LPWSTR textRaw = nullptr;
            if (SUCCEEDED(args->TryGetWebMessageAsString(&textRaw)) && textRaw) {
              CoTaskMemFree(textRaw);
              return S_OK;
            }
            LPWSTR raw = nullptr;
            if (FAILED(args->get_WebMessageAsJson(&raw)) || !raw) return S_OK;
            const std::wstring message(raw);
            CoTaskMemFree(raw);
            try {
              const auto object = winrt::Windows::Data::Json::JsonObject::Parse(message);
              const std::wstring type = object.GetNamedString(L"type", L"").c_str();
              if (type != L"stationhead-auth-probe") return S_OK;
              authProbeInFlight_ = false;
              authProbeStartedAt_ = 0;
              const std::wstring state = object.GetNamedString(L"state", L"").c_str();
              if (state == L"auth-failed") {
                loginRequired_ = true;
                ShowInteractive(true);
                SetStatus(L"secondary Stationhead authentication expired");
                log_.Warn(L"Secondary Stationhead authentication probe rejected with HTTP " +
                          std::to_wstring(static_cast<int>(object.GetNamedNumber(L"status", 0))));
                PostMessageW(window_, WM_HP_STATIONHEAD_CHANGED, 0, 0);
              } else if (state == L"ok") {
                SetStatus(L"secondary Stationhead authentication probe ok");
                PostMessageW(window_, WM_HP_STATIONHEAD_CHANGED, 0, 0);
              } else if (state == L"no-auth-header") {
                SetStatus(L"secondary Stationhead auth probe waiting for session");
              } else {
                SetStatus(L"secondary Stationhead auth probe failed");
                log_.Warn(L"Secondary Stationhead auth probe returned an error");
              }
            } catch (...) {
              authProbeInFlight_ = false;
              authProbeStartedAt_ = 0;
              log_.Warn(L"Secondary Stationhead auth probe message was invalid");
            }
            return S_OK;
          }).Get(), &authProbeMessageToken_);
  webview_->add_NavigationCompleted(
      Callback<ICoreWebView2NavigationCompletedEventHandler>(
          [this, alive](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            if (!CallbackAlive(alive) || shuttingDown_) return S_OK;
            BOOL success = FALSE;
            if (args) args->get_IsSuccess(&success);
            {
              std::lock_guard lock(mutex_);
              status_.navigating = false;
              status_.detail = success ? L"secondary station loaded" : L"secondary station navigation failed";
            }
            if (success) {
              lastReloadAt_ = UnixMillis();
              lastAuthProbeAt_ = lastReloadAt_;
              authProbeStartedAt_ = 0;
              authProbeInFlight_ = false;


              appliedVolumePercent_.store(-1, std::memory_order_relaxed);
              ApplyAudioState();
            } else {
              ScheduleRetry(L"navigation failed", 5'000);
            }
            return S_OK;
          }).Get(), &navigationToken_);
  webview_->add_ProcessFailed(
      Callback<ICoreWebView2ProcessFailedEventHandler>(
          [this, alive](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) -> HRESULT {
            if (!CallbackAlive(alive) || shuttingDown_) return S_OK;
            {
              std::lock_guard lock(mutex_);
              status_.processFailed = true;
              status_.detail = L"WebView2 process failed";
            }
            ScheduleRetry(L"process failed", 5'000);
            return S_OK;
          }).Get(), &processFailedToken_);
}

}  // namespace hp
