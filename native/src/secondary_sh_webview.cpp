// Part of secondary_sh.cpp's translation unit (see the #include at the end of
// that file). Main WebView2 configuration for the secondary Stationhead player:
// settings, resource blocking, the injected kStartupScript, and the new-window /
// message / navigation / process-failed handlers. Uses the anonymous-namespace
// helpers (kStartupScript, kProfileName, HResultHex, CallbackAlive) from
// secondary_sh.cpp.
#include "secondary_sh.h"
#include "sh_shared.h"

namespace hp {

void SecondaryStationheadPlayer::ConfigureWebView() {
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
  ComPtr<ICoreWebView2_19> v19;
  if (config_.lowMemoryMode && SUCCEEDED(webview_.As(&v19))) {
    v19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
  }
  ComPtr<ICoreWebView2Controller2> controller2;
  if (SUCCEEDED(controller_.As(&controller2))) {
    COREWEBVIEW2_COLOR background{255, 0, 0, 0};
    controller2->put_DefaultBackgroundColor(background);
  }
  webview_->AddScriptToExecuteOnDocumentCreated(
      kStartupScript,
      Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
          [this, alive](HRESULT result, LPCWSTR) -> HRESULT {
            if (CallbackAlive(alive) && FAILED(result)) log_.Warn(L"Secondary Stationhead startup script registration failed");
            return S_OK;
          }).Get());
  webview_->add_NewWindowRequested(
      Callback<ICoreWebView2NewWindowRequestedEventHandler>(
          [this, alive](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            if (!args) return S_OK;
            // Always mark the request handled so a failure below never falls through to
            // WebView2's default behavior of opening an uncontrolled top-level popup window.
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
                    [this, alive, authAlive, popupArgs, deferral, uri](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
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
            const int64_t now = UnixMillis();
            if (message == L"secondary-playing") {
              audioPlaying_ = true;
              resourceBlockingArmed_ = true;
              lastAudioAt_ = now;
              audioStoppedAt_ = 0;
              retryAt_ = 0;
              const bool wasLoginInteractive = loginRequired_.exchange(false, std::memory_order_acq_rel);
              if ((wasLoginInteractive || interactive_) && !spotifyAuthorization_) ShowInteractive(false);
              SetStatus(L"audio detected");
            } else if (message == L"secondary-stopped") {
              audioPlaying_ = false;
              if (audioStoppedAt_ == 0) audioStoppedAt_ = now;
              if (!spotifyAuthorization_) ShowInteractive(true);
            } else if (message == L"secondary-login-required") {
              loginRequired_ = true;
              ShowInteractive(true);
              SetStatus(L"login required in secondary profile");
            }
            return S_OK;
          }).Get(), &messageToken_);
  webview_->add_NavigationCompleted(
      Callback<ICoreWebView2NavigationCompletedEventHandler>(
          [this, alive](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            if (!CallbackAlive(alive) || shuttingDown_) return S_OK;
            if (apiAuthorization_) return S_OK;
            BOOL success = FALSE;
            if (args) args->get_IsSuccess(&success);
            {
              std::lock_guard lock(mutex_);
              status_.navigating = false;
              status_.detail = success ? L"secondary station loaded" : L"secondary station navigation failed";
            }
            if (success) {
              lastReloadAt_ = UnixMillis();
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
  SetStartupBounds();
  createdAt_ = UnixMillis();
  lastReloadAt_ = createdAt_;
  {
    std::lock_guard lock(mutex_);
    status_.created = true;
    status_.navigating = true;
    status_.processFailed = false;
    status_.url = config_.secondaryUrl;
    status_.detail = L"loading secondary station";
  }
  const HRESULT result = webview_->Navigate(config_.secondaryUrl.c_str());
  if (FAILED(result)) {
    ScheduleRetry(L"initial navigation failed " + HResultHex(result), 1'000);
    return;
  }
  log_.Info(L"Secondary Stationhead started with isolated profile: " + config_.secondaryUrl);
}

}  // namespace hp
