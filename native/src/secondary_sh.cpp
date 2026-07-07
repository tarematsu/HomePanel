#include "secondary_sh.h"
#include "sh_shared.h"

namespace hp {
namespace {
constexpr wchar_t kProfileName[] = L"stationhead-secondary";
constexpr int64_t kAudioRecoveryMs = 10'000;
constexpr int64_t kNavigationTimeoutMs = 60'000;
constexpr int64_t kApiAuthTimeoutMs = 10 * 60'000;

bool CallbackAlive(const std::shared_ptr<std::atomic<bool>>& alive) {
  return alive && alive->load(std::memory_order_acquire);
}

bool WindowOwnsFocus(HWND host, HWND focused) {
  return host && focused && IsWindow(host) && (host == focused || IsChild(host, focused));
}

std::wstring HResultHex(HRESULT value) {
  std::wostringstream output;
  output << L"0x" << std::hex << static_cast<unsigned long>(value);
  return output.str();
}

constexpr wchar_t kStartupScript[] = LR"JS(
(() => {
  const host = String(location.hostname || '').toLowerCase();
  if (host !== 'stationhead.com' && !host.endsWith('.stationhead.com')) return;
  if (window.__homepanelSecondaryStationhead) return;
  const nativeTimeout = window.setTimeout.bind(window);
  const NativeMutationObserver = window.MutationObserver;
  const normalize = value => String(value || '').replace(/\s+/g, ' ').trim();
  const selector = "button,[role='button'],a,input[type='button'],input[type='submit'],[aria-label],[data-testid],[tabindex]";
  const startPattern = /\b(start|join|resume)\s+(listening|station|show|room)\b|\blisten\s+(now|live)\b/i;
  const loginPattern = /^(log\s*in|sign\s*in|login)(?:\s+.*)?$/i;
  let observer = null;
  let scanQueued = false;
  let scanTimer = 0;
  let attempts = 0;
  let retryAt = 0;
  let loginReported = false;
  let lastTargetSignature = '';
  let lastPlaying = null;
  const observedAt = Date.now();
  const labelOf = element => [
    element?.innerText,
    element?.getAttribute?.('aria-label'),
    element?.textContent,
    element?.getAttribute?.('title'),
    element?.getAttribute?.('value'),
    element?.getAttribute?.('data-testid')
  ].map(normalize).find(Boolean) || '';
  const visible = element => {
    if (!element || element.disabled || element.getAttribute?.('aria-disabled') === 'true' ||
        element.getAttribute?.('aria-hidden') === 'true') return false;
    const rect = element.getBoundingClientRect?.();
    if (!rect || rect.width <= 2 || rect.height <= 2) return false;
    const style = getComputedStyle(element);
    return style.display !== 'none' && style.visibility !== 'hidden' &&
      Number(style.opacity || 1) > 0 && style.pointerEvents !== 'none';
  };
  const playing = () => {
    if (navigator.mediaSession?.playbackState === 'playing') return true;
    return Array.from(document.querySelectorAll('audio,video')).some(element =>
      !element.paused && !element.ended && element.readyState >= 2);
  };
  const publishAudio = () => {
    const current = playing();
    if (current === lastPlaying) return current;
    lastPlaying = current;
    try { window.chrome?.webview?.postMessage(current ? 'secondary-playing' : 'secondary-stopped'); } catch (_) {}
    return current;
  };
  const scan = () => {
    scanQueued = false;
    scanTimer = 0;
    const ready = document.readyState !== 'loading' && !!document.body;
    const isPlaying = publishAudio();
    if (!ready || !document.body) return;
    if (isPlaying) {
      observer?.disconnect?.();
      observer = null;
      return;
    }
    let start = null;
    let login = false;
    for (const element of document.querySelectorAll(selector)) {
      if (!visible(element)) continue;
      const label = labelOf(element);
      if (!start && startPattern.test(label)) start = element;
      if (!login && loginPattern.test(label)) login = true;
      if (start && login) break;
    }
    if (start && attempts < 2 && Date.now() >= retryAt) {
      const target = start.closest?.("button,[role='button'],a,input[type='button'],input[type='submit'],[tabindex]") || start;
      const rect = target.getBoundingClientRect();
      const signature = `${labelOf(target)}:${Math.round(rect.left)}:${Math.round(rect.top)}`;
      if (signature !== lastTargetSignature) {
        lastTargetSignature = signature;
        attempts = 0;
        retryAt = 0;
      }
      attempts += 1;
      retryAt = Date.now() + 1500;
      try { target.click?.(); } catch (_) {}
      if (attempts < 2) nativeTimeout(schedule, 1500);
    } else if (!start && login && !loginReported && Date.now() - observedAt >= 15000) {
      loginReported = true;
      try { window.chrome?.webview?.postMessage('secondary-login-required'); } catch (_) {}
    }
  };
  const schedule = () => {
    if (scanQueued) return;
    scanQueued = true;
    scanTimer = nativeTimeout(scan, 250);
  };
  const relevant = record => {
    if (record.type === 'attributes') return Boolean(record.target?.matches?.(selector) || record.target?.closest?.(selector));
    if (record.type === 'characterData') return Boolean(record.target?.parentElement?.closest?.(selector));
    return [...(record.addedNodes || []), ...(record.removedNodes || [])].some(node =>
      node instanceof Element && (node.matches?.(selector) || node.querySelector?.(selector)));
  };
  window.__homepanelSecondaryStationhead = { scan: schedule };
  if (NativeMutationObserver) {
    observer = new NativeMutationObserver(records => { if (records.some(relevant)) schedule(); });
    observer.observe(document, {
      childList: true, subtree: true
    });
  }
  for (const eventName of ['play','playing','canplay','pause','ended','stalled','waiting','error']) {
    document.addEventListener(eventName, publishAudio, true);
  }
  document.addEventListener('DOMContentLoaded', schedule, { once: true });
  window.addEventListener('load', schedule, { once: true });
  schedule();
  nativeTimeout(schedule, 15000);
})()
)JS";
}  // namespace

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
  StopSpotifyApiAuthorizationWorker();
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
  copy.lightweight = false;
  copy.loginRequired = loginRequired_;
  copy.audioMuted = audioMuted_.load(std::memory_order_relaxed);
  return copy;
}

void SecondaryStationheadPlayer::SetStatus(const std::wstring& detail) {
  std::lock_guard lock(mutex_);
  status_.detail = detail;
}

bool SecondaryStationheadPlayer::EnsureHostWindow() {
  if (hostWindow_ && IsWindow(hostWindow_)) return true;
  if (!window_ || !IsWindow(window_)) return false;
  static constexpr wchar_t kClassName[] = L"HomePanelSecondaryStationheadHost";
  static std::once_flag classOnce;
  std::call_once(classOnce, [] {
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&windowClass);
  });
  const int width = std::max(1L, bounds_.right - bounds_.left);
  const int height = std::max(1L, bounds_.bottom - bounds_.top);
  hostWindow_ = CreateWindowExW(0, kClassName, L"SecondaryStationheadHost",
      WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, bounds_.left, bounds_.top,
      width, height, window_, nullptr, GetModuleHandleW(nullptr), nullptr);
  return hostWindow_ && IsWindow(hostWindow_);
}

bool SecondaryStationheadPlayer::EnsureAuthHostWindow() {
  if (authHostWindow_ && IsWindow(authHostWindow_)) return true;
  if (!window_ || !IsWindow(window_)) return false;
  static constexpr wchar_t kClassName[] = L"HomePanelSecondarySpotifyAuthHost";
  static std::once_flag classOnce;
  std::call_once(classOnce, [] {
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&windowClass);
  });
  const int width = std::max(1L, bounds_.right - bounds_.left);
  const int height = std::max(1L, bounds_.bottom - bounds_.top);
  authHostWindow_ = CreateWindowExW(0, kClassName, L"SecondarySpotifyAuthHost",
      WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, bounds_.left, bounds_.top,
      width, height, window_, nullptr, GetModuleHandleW(nullptr), nullptr);
  return authHostWindow_ && IsWindow(authHostWindow_);
}

void SecondaryStationheadPlayer::SetBounds(const RECT& bounds) {
  if (EqualRect(&bounds_, &bounds)) return;
  bounds_ = bounds;
  LayoutWindows(interactive_ || spotifyAuthorization_ || loginRequired_.load(std::memory_order_relaxed));
}

void SecondaryStationheadPlayer::LayoutWindows(bool interactive) {
  const bool wasInteractive = interactive_;
  const bool authWasVisible = authHostWindow_ && IsWindow(authHostWindow_) && IsWindowVisible(authHostWindow_);
  const int width = std::max(1L, bounds_.right - bounds_.left);
  const int height = std::max(1L, bounds_.bottom - bounds_.top);
  const RECT controllerBounds{0, 0, width, height};
  const bool showAuth = interactive && spotifyAuthorization_ && authController_;
  if (controller_) {
    controller_->put_Bounds(controllerBounds);
    controller_->put_IsVisible(showAuth ? FALSE : TRUE);
  }
  if (hostWindow_ && IsWindow(hostWindow_)) {
    if (showAuth) {
      ShowWindow(hostWindow_, SW_HIDE);
    } else {
      ShowWindow(hostWindow_, SW_SHOWNOACTIVATE);
      SetWindowPos(hostWindow_, interactive ? HWND_TOP : HWND_BOTTOM,
                   bounds_.left, bounds_.top, width, height,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
  }
  if (authController_) {
    authController_->put_Bounds(controllerBounds);
    authController_->put_IsVisible(showAuth ? TRUE : FALSE);
  }
  if (authHostWindow_ && IsWindow(authHostWindow_)) {
    if (showAuth) {
      ShowWindow(authHostWindow_, SW_SHOWNOACTIVATE);
      SetWindowPos(authHostWindow_, HWND_TOP, bounds_.left, bounds_.top, width, height,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
      ShowWindow(authHostWindow_, SW_HIDE);
    }
  }
  interactive_ = interactive;
  {
    std::lock_guard lock(mutex_);
    status_.visible = interactive;
    status_.spotifyAuthorization = spotifyAuthorization_;
    status_.apiAuthorization = apiAuthorization_;
  }
  if (interactive && (!wasInteractive || (showAuth && !authWasVisible))) {
    HWND target = showAuth ? authHostWindow_ : hostWindow_;
    if (target && IsWindow(target)) SetFocus(target);
    if (showAuth && authController_) authController_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    else if (controller_) controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
  }
}

void SecondaryStationheadPlayer::ShowInteractive(bool interactive) {
  LayoutWindows(interactive || spotifyAuthorization_ || loginRequired_.load(std::memory_order_acquire));
}

void SecondaryStationheadPlayer::SetStartupBounds() {
  ShowInteractive(false);
}

void SecondaryStationheadPlayer::FinishSpotifyAuthorization(const std::wstring& detail) {
  spotifyAuthorization_ = false;
  authClosePending_ = true;
  SetStatus(detail);
}

void SecondaryStationheadPlayer::ConfigureAuthWebView() {
  const auto alive = callbackAlive_;
  const auto authAlive = authCallbackAlive_;
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
            if (!CallbackAlive(alive) || shuttingDown_ || !args || !environment_) return S_OK;
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
              if (wasLoginInteractive && !spotifyAuthorization_) ShowInteractive(false);
              SetStatus(L"audio detected");
            } else if (message == L"secondary-stopped") {
              audioPlaying_ = false;
              if (audioStoppedAt_ == 0) audioStoppedAt_ = now;
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

void SecondaryStationheadPlayer::Reconnect() {
  if (!webview_) {
    if (!creating_) Create();
    return;
  }
  audioPlaying_ = false;
  lastAudioAt_ = 0;
  audioStoppedAt_ = 0;
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
  createdAt_ = UnixMillis();
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
  if (apiAuthorization_ && apiAuthStartedAt_ > 0 && !apiAuthExchangePending_ &&
      nowMs - apiAuthStartedAt_ >= kApiAuthTimeoutMs) {
    log_.Warn(L"Spotify API authorization timed out; restoring secondary Stationhead");
    ResetSpotifyApiAuthorization(L"Spotify API authentication timed out; returning to buddy46", false);
    RestoreSecondaryAfterSpotifyApiAuthorization();
    return;
  }
  if (!webview_) {
    nextTickAt_ = nowMs + 1'000;
    return;
  }
  MaybeStartSpotifyApiAuthorization(this);
  if (!audioPlaying_.load(std::memory_order_relaxed) && audioStoppedAt_ > 0 &&
      nowMs - audioStoppedAt_ >= kAudioRecoveryMs && !loginRequired_) {
    log_.Warn(L"Secondary Stationhead audio stopped; reconnecting");
    Reconnect();
    return;
  }
  if (retryAt_ == 0 && !loginRequired_ && createdAt_ > 0 &&
      nowMs - createdAt_ >= kNavigationTimeoutMs && !audioPlaying_) {
    ScheduleRetry(L"audio startup timeout", 5'000);
    return;
  }
  const int64_t reloadInterval = StationheadReloadIntervalMs(std::max(5, config_.secondaryReloadIntervalMinutes));
  if (audioPlaying_.load(std::memory_order_relaxed) && lastReloadAt_ > 0 &&
      nowMs - lastReloadAt_ >= reloadInterval) {
    log_.Info(L"Secondary Stationhead maintenance reload");
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
  if (apiAuthorization_ && apiAuthStartedAt_ > 0) consider(apiAuthStartedAt_ + kApiAuthTimeoutMs);
  if (!audioPlaying_.load(std::memory_order_relaxed) && audioStoppedAt_ > 0 && !loginRequired_) {
    consider(audioStoppedAt_ + kAudioRecoveryMs);
  }
  if (retryAt_ == 0 && !loginRequired_ && createdAt_ > 0 && !audioPlaying_) {
    consider(createdAt_ + kNavigationTimeoutMs);
  }
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
  if (apiAuthorization_) ResetSpotifyApiAuthorization(L"", false);
  callbackAlive_->store(false, std::memory_order_release);
  authCallbackAlive_->store(false, std::memory_order_release);
  CloseAuthWebView();
  if (webview_) {
    if (navigationToken_.value) webview_->remove_NavigationCompleted(navigationToken_);
    if (newWindowToken_.value) webview_->remove_NewWindowRequested(newWindowToken_);
    if (messageToken_.value) webview_->remove_WebMessageReceived(messageToken_);
    if (processFailedToken_.value) webview_->remove_ProcessFailed(processFailedToken_);
    if (resourceRequestedToken_.value) webview_->remove_WebResourceRequested(resourceRequestedToken_);
  }
  navigationToken_ = {};
  newWindowToken_ = {};
  messageToken_ = {};
  processFailedToken_ = {};
  resourceRequestedToken_ = {};
  resourceBlockingArmed_ = false;
  if (controller_) controller_->Close();
  webview_.Reset();
  controller_.Reset();
  environment_.Reset();
  if (hostWindow_ && IsWindow(hostWindow_)) ShowWindow(hostWindow_, SW_HIDE);
  if (authHostWindow_ && IsWindow(authHostWindow_)) ShowWindow(authHostWindow_, SW_HIDE);
  creating_ = false;
  interactive_ = false;
  spotifyAuthorization_ = false;
  authClosePending_ = false;
  audioPlaying_ = false;
  loginRequired_ = false;
  createdAt_ = 0;
  lastAudioAt_ = 0;
  audioStoppedAt_ = 0;
  lastReloadAt_ = 0;
  retryAt_ = 0;
  nextTickAt_ = 0;
  {
    std::lock_guard lock(mutex_);
    status_.created = false;
    status_.navigating = false;
    status_.playing = false;
    status_.lightweight = false;
    status_.loginRequired = false;
    status_.spotifyAuthorization = false;
    status_.visible = false;
    status_.processFailed = false;
  }
}

}  // namespace hp
