#include "sh.h"
#include "shared_webview_environment.h"
#include "sh_shared.h"

namespace hp {
namespace {
constexpr int64_t kDailyPlayStatsIntervalMs = 5 * 60'000;
constexpr int64_t kAuthProbeIntervalMs = 5 * 60'000;
constexpr int64_t kAuthProbeTimeoutMs = 30'000;
constexpr int64_t kStationheadTrackBoundaryRefreshDelayMs = 52 * 60'000;
constexpr int64_t kStationheadTrackBoundaryNavigationTimeoutMs = 30'000;
constexpr int64_t kStationheadTrackBoundaryPlaybackRecoveryTimeoutMs = 30'000;
constexpr size_t kStationheadDiagnosticsMaxCharacters = 2'048;

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
      userDataFolder_(std::move(userDataFolder)),
      profileName_(role == StationheadRole::Secondary ? L"stationhead-secondary" : L"Default"),
      log_(log) {
  if (IsSecondary()) status_.url = config_.secondaryUrl;
}

StationheadPlayer::~StationheadPlayer() { Stop(); }

void StationheadPlayer::Start() {
  shuttingDown_ = false;
  usingFallback_ = false;
  trackBoundaryPlaybackRecoveryPending_ = false;
  trackBoundaryPlaybackRecoveryAwaitingNavigation_ = false;
  trackBoundaryPlaybackRecoveryDeadline_ = 0;
  ResetNavigationRouteState();
  Create();
}

void StationheadPlayer::Stop() {
  shuttingDown_ = true;
  audioPlayingSinceAt_.store(0, std::memory_order_relaxed);
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
  audioPlayingSinceAt_.store(0, std::memory_order_relaxed);
  trackBoundaryRefreshPending_ = false;
  nextTickAt_ = 0;
  nextAutoClickAt_ = 0;
}

void StationheadPlayer::ApplyAudioPlaybackState(bool playing, const std::wstring& source) {
  const bool awaitingTrackBoundaryNavigation =
      trackBoundaryPlaybackRecoveryPending_ &&
      trackBoundaryPlaybackRecoveryAwaitingNavigation_;
  const bool changed =
      audioPlaying_.exchange(playing, std::memory_order_relaxed) != playing;
  // Give the page's own auto-click scanners the same ground truth WebView2's
  // native audio detection already established, instead of leaving them to
  // infer "is it playing" from page-reported signals (like mediaSession
  // metadata) that a site can set before audio is actually audible - which
  // would make the scanners think Start Listening was already handled and
  // stop trying to press it.
  if (webview_) {
    webview_->ExecuteScript(
        playing ? L"window.__homepanelAudioPlaying = true;"
                : L"window.__homepanelAudioPlaying = false;",
        nullptr);
  }
  if (playing) {
    // WebView2 may report audio from the outgoing document before the matching
    // NavigationCompleted event. Keep the navigation watchdog armed and avoid
    // treating that early signal as recovery until the new document is known
    // to be committed.
    if (awaitingTrackBoundaryNavigation) {
      audioPlayingSinceAt_.store(0, std::memory_order_relaxed);
      {
        std::lock_guard lock(mutex_);
        status_.audioPlaying = true;
        status_.playing = true;
        status_.loginRequired = false;
        status_.detail = L"audio observed while track-boundary navigation is still pending";
      }
      if (changed) {
        log_.Info(L"Stationhead " + std::wstring(RoleTag()) +
                  L" audio observed before track-boundary navigation completed (" +
                  source + L")");
      }
      nextTickAt_ = 0;
      PostChange();
      return;
    }

    if (audioPlayingSinceAt_.load(std::memory_order_relaxed) <= 0) {
      audioPlayingSinceAt_.store(UnixMillis(), std::memory_order_relaxed);
    }
    if (trackBoundaryPlaybackRecoveryPending_) {
      trackBoundaryPlaybackRecoveryPending_ = false;
      trackBoundaryPlaybackRecoveryAwaitingNavigation_ = false;
      trackBoundaryPlaybackRecoveryDeadline_ = 0;
      log_.Info(L"Stationhead " + std::wstring(RoleTag()) +
                L" audio recovered after track-boundary refresh");
    }
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
    if (changed) log_.Info(L"Stationhead " + std::wstring(RoleTag()) + L" audio playing (" + source + L")");
    PostChange(StationheadChangeReturnMain);
    return;
  }

  audioPlayingSinceAt_.store(0, std::memory_order_relaxed);
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
  if (changed) {
    nextAutoClickAt_ = UnixMillis() + kStationheadPostPlaybackStopClickDelayMs;
    log_.Warn(L"Stationhead " + std::wstring(RoleTag()) + L" audio stopped (" + source + L")");
  }
  // Don't reveal the player here: a stop can be a brief track-transition gap,
  // and the App layer's RefreshVisibility()/SelectTab(None) calls (which know
  // about the transition grace period) already re-evaluate visibility on the
  // next tick. Showing it unconditionally on every stop made the window pop
  // to the front for an instant on every track change.
  PostChange();
}

void StationheadPlayer::NavigateCurrentUrl(int64_t nowMs, const std::wstring& reason) {
  NavigateStationheadUrl(nowMs, CurrentStationheadUrl(), reason, usingFallback_);
}

void StationheadPlayer::HandleTrackEnded(int64_t nowMs, bool retry) {
  if (!retry) trackBoundaryRefreshPending_ = false;
  if (!webview_ || spotifyAuthorization_ || loginRequired_ ||
      recreating_.load(std::memory_order_relaxed) ||
      navigationInFlight_.load(std::memory_order_acquire) || lastReloadAt_ <= 0) {
    trackBoundaryRefreshPending_ = false;
    return;
  }
  {
    std::lock_guard lock(mutex_);
    if (status_.navigating) {
      trackBoundaryRefreshPending_ = false;
      return;
    }
  }

  if (retry) {
    if (!trackBoundaryRefreshPending_) return;
  } else {
    if (nowMs - lastReloadAt_ < kStationheadTrackBoundaryRefreshDelayMs) return;
    trackBoundaryRefreshPending_ = true;
  }

  if (!window_ || !IsWindow(window_)) {
    trackBoundaryRefreshPending_ = false;
    return;
  }
  const UINT readyMessage = IsSecondary()
      ? WM_HP_SECONDARY_RELOAD_READY
      : WM_HP_PRIMARY_RELOAD_READY;
  if (SendMessageW(window_, readyMessage, 0, 0) == 0) {
    log_.Info(L"Stationhead " + std::wstring(RoleTag()) +
              L" track-boundary refresh waiting for stable handoff audio");
    return;
  }

  trackBoundaryRefreshPending_ = false;
  lastReloadAt_ = nowMs;
  NavigateCurrentUrl(nowMs, L"track-boundary authentication refresh");
}

void StationheadPlayer::RecoverTrackBoundaryPlayback() {
  if (!trackBoundaryPlaybackRecoveryPending_ ||
      trackBoundaryPlaybackRecoveryAwaitingNavigation_ || !webview_ ||
      spotifyAuthorization_ || loginRequired_ ||
      recreating_.load(std::memory_order_relaxed) ||
      navigationInFlight_.load(std::memory_order_acquire)) {
    return;
  }
  if (audioPlaying_.load(std::memory_order_relaxed)) {
    trackBoundaryPlaybackRecoveryPending_ = false;
    trackBoundaryPlaybackRecoveryDeadline_ = 0;
    log_.Info(L"Stationhead " + std::wstring(RoleTag()) +
              L" playback recovery watchdog observed audio before rebuilding");
    return;
  }

  trackBoundaryPlaybackRecoveryPending_ = false;
  trackBoundaryPlaybackRecoveryAwaitingNavigation_ = false;
  trackBoundaryPlaybackRecoveryDeadline_ = 0;
  const auto alive = createCallbackAlive_;
  ComPtr<ICoreWebView2> view = webview_;
  static constexpr wchar_t kDiagnosticsScript[] = LR"JS(
(() => {
  const sourceHost = element => {
    try {
      const source = element.currentSrc || element.src || '';
      return source ? new URL(source, location.href).hostname : '';
    } catch (_) {
      return '';
    }
  };
  // Failure diagnostics have a strict observability budget: sampling a bounded
  // number of media elements avoids allocating and serializing an unbounded DOM
  // snapshot immediately before the WebView is rebuilt.
  const elements = document.querySelectorAll('audio,video');
  const sampledCount = Math.min(elements.length, 8);
  const media = [];
  for (let index = 0; index < sampledCount; index += 1) {
    const element = elements[index];
    media.push({
      index,
      tag: element.tagName,
      paused: element.paused,
      ended: element.ended,
      readyState: element.readyState,
      networkState: element.networkState,
      currentTime: Number.isFinite(element.currentTime) ? element.currentTime : null,
      duration: Number.isFinite(element.duration) ? element.duration : null,
      muted: element.muted,
      volume: element.volume,
      hasMediaKeys: Boolean(element.mediaKeys),
      error: element.error ? {
        code: element.error.code,
        message: String(element.error.message || '').slice(0, 160),
      } : null,
      sourceHost: sourceHost(element),
    });
  }
  const diagnostics = {
    locationHost: String(location.hostname || ''),
    locationPath: String(location.pathname || '').slice(0, 256),
    visibility: document.visibilityState,
    focused: document.hasFocus(),
    mediaSession: navigator.mediaSession?.playbackState || '',
    audioFlag: window.__homepanelAudioPlaying,
    mediaCount: elements.length,
    omittedMediaCount: Math.max(0, elements.length - sampledCount),
    media,
  };
  // Leave headroom for WebView2's result encoding and the native log prefix.
  // Preserve the first elements and exact total/omitted counts when trimming.
  while (diagnostics.media.length > 1 &&
         JSON.stringify(diagnostics).length > 1_800) {
    diagnostics.media.pop();
    diagnostics.omittedMediaCount += 1;
  }
  return diagnostics;
})()
)JS";
  const HRESULT diagnosticStarted = view->ExecuteScript(
      kDiagnosticsScript,
      Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
          [this, alive, view](HRESULT result, LPCWSTR resultJson) -> HRESULT {
            if (!CallbackAlive(alive) || view.Get() != webview_.Get()) return S_OK;
            if (FAILED(result)) {
              log_.Warn(L"Stationhead " + std::wstring(RoleTag()) +
                        L" track-boundary playback diagnostics failed " +
                        HResultHex(result));
              return S_OK;
            }
            std::wstring diagnostic = resultJson ? resultJson : L"null";
            if (diagnostic.size() > kStationheadDiagnosticsMaxCharacters) {
              diagnostic.resize(kStationheadDiagnosticsMaxCharacters);
            }
            log_.Warn(L"Stationhead " + std::wstring(RoleTag()) +
                      L" track-boundary playback diagnostics: " + diagnostic);
            return S_OK;
          }).Get());
  if (FAILED(diagnosticStarted)) {
    log_.Warn(L"Stationhead " + std::wstring(RoleTag()) +
              L" track-boundary diagnostics could not start " +
              HResultHex(diagnosticStarted));
  }
  ScheduleRecreate(
      L"audio did not recover after track-boundary refresh; rebuilding DRM playback session",
      2'000);
}

void StationheadPlayer::TryStartInitialNavigation() {
  if (!webViewConfigured_ || !authCaptureScriptRegistrationComplete_ ||
      !startupScriptRegistrationComplete_ || startupNavigationStarted_ ||
      shuttingDown_ || recreating_.load(std::memory_order_relaxed) || !webview_) {
    return;
  }
  startupNavigationStarted_ = true;
  startupScriptDeadline_ = 0;
  log_.Info(L"Stationhead " + std::wstring(RoleTag()) +
            L" startup prerequisites ready; starting initial navigation");
  if (!pendingAuthorizationUrl_.empty()) {
    const std::wstring authorizationUrl = pendingAuthorizationUrl_;
    pendingAuthorizationUrl_.clear();
    OpenSpotifyAuthorization(authorizationUrl);
  } else {
    NavigateCurrentUrl(UnixMillis(), L"startup");
  }
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
  if (!webview_ || url.empty()) return;
  const bool trackBoundaryRefresh =
      reason == L"track-boundary authentication refresh";
  trackBoundaryPlaybackRecoveryPending_ = trackBoundaryRefresh;
  trackBoundaryPlaybackRecoveryAwaitingNavigation_ = trackBoundaryRefresh;
  trackBoundaryPlaybackRecoveryDeadline_ = trackBoundaryRefresh
      ? nowMs + kStationheadTrackBoundaryNavigationTimeoutMs
      : 0;
  stationNavigationStarted_ = true;
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
    trackBoundaryPlaybackRecoveryPending_ = false;
    trackBoundaryPlaybackRecoveryAwaitingNavigation_ = false;
    trackBoundaryPlaybackRecoveryDeadline_ = 0;
    ScheduleRecreate(L"navigation start failed " + HResultHex(result), 1'000);
    return;
  }
  if (trackBoundaryRefresh) {
    log_.Info(L"Stationhead " + std::wstring(RoleTag()) +
              L" armed 30-second navigation watchdog for track-boundary refresh");
  }
  log_.Info(L"Stationhead " + std::wstring(RoleTag()) + L" navigation (" + reason + L"): " + url);
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

// Locates the Start Listening control and clicks it entirely from native
// code: a page-side signal (or the periodic Tick() retry below) only tells
// us a candidate is probably visible, then this re-locates it fresh right
// before dispatching a trusted CDP click, so the coordinates can never go
// stale between detection and dispatch the way an earlier page-computed
// position could.
void StationheadPlayer::AttemptNativeStartClick(int64_t nowMs) {
  if (!webview_ || autoClickInFlight_ ||
      navigationInFlight_.load(std::memory_order_acquire) ||
      recreating_.load(std::memory_order_relaxed) ||
      audioPlaying_.load(std::memory_order_relaxed) || nowMs < nextAutoClickAt_) {
    return;
  }
  nextAutoClickAt_ = nowMs + kStationheadAutoClickRetryMs;
  autoClickInFlight_ = true;
  const auto alive = createCallbackAlive_;
  const uint64_t navigationIdAtStart =
      activeNavigationId_.load(std::memory_order_acquire);
  static const std::wstring locateScript = StationheadLocateStartButtonScript();
  ComPtr<ICoreWebView2> view = webview_;
  const HRESULT result = webview_->ExecuteScript(
      locateScript.c_str(),
      Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
          [this, alive, view, navigationIdAtStart](
              HRESULT scriptResult, LPCWSTR resultJson) -> HRESULT {
            if (!CallbackAlive(alive)) return S_OK;
            const auto navigationChanged = [this, navigationIdAtStart]() noexcept {
              return navigationInFlight_.load(std::memory_order_acquire) ||
                  activeNavigationId_.load(std::memory_order_acquire) !=
                      navigationIdAtStart;
            };
            if (view.Get() != webview_.Get() || navigationChanged() ||
                FAILED(scriptResult) || !resultJson) {
              autoClickInFlight_ = false;
              return S_OK;
            }
            double x = 0.0;
            double y = 0.0;
            if (!ParseStationheadLocateButtonResult(resultJson, x, y) ||
                navigationChanged()) {
              autoClickInFlight_ = false;
              return S_OK;
            }
            nextAutoClickAt_ = std::max(
                nextAutoClickAt_, UnixMillis() + kStationheadAutoClickSuccessGraceMs);
            log_.Info(L"Stationhead " + std::wstring(RoleTag()) +
                      L" auto-clicking Start Listening at " + std::to_wstring(x) +
                      L"," + std::to_wstring(y));
            DispatchStationheadNativeClick(view.Get(), x, y, log_, RoleTag());
            autoClickInFlight_ = false;
            return S_OK;
          }).Get());
  if (FAILED(result)) autoClickInFlight_ = false;
}

HRESULT StationheadPlayer::CreateProfileController(
    HWND parentWindow,
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* handler) const noexcept {
  if (!environment_ || !parentWindow || !handler) return E_INVALIDARG;
  ComPtr<ICoreWebView2Environment10> environment10;
  HRESULT result = environment_.As(&environment10);
  if (FAILED(result) || !environment10) return E_NOINTERFACE;

  ComPtr<ICoreWebView2ControllerOptions> options;
  result = environment10->CreateCoreWebView2ControllerOptions(&options);
  if (FAILED(result) || !options) return FAILED(result) ? result : E_FAIL;
  result = options->put_ProfileName(profileName_.c_str());
  if (FAILED(result)) return result;
  result = options->put_IsInPrivateModeEnabled(FALSE);
  if (FAILED(result)) return result;
  return environment10->CreateCoreWebView2ControllerWithOptions(
      parentWindow, options.Get(), handler);
}

void StationheadPlayer::Create() {
  if (shuttingDown_ || creating_.exchange(true)) return;
  creationStartedAt_ = UnixMillis();
  if (!EnsureHostWindow()) {
    creating_ = false;
    creationStartedAt_ = 0;
    ScheduleRecreate(L"main window unavailable");
    return;
  }
  createCallbackAlive_->store(false, std::memory_order_release);
  createCallbackAlive_ = std::make_shared<std::atomic<bool>>(true);
  autoClickInFlight_ = false;
  const auto alive = createCallbackAlive_;
  SharedWebViewEnvironment::Instance().Acquire(
      userDataFolder_, [this, alive](HRESULT result, ICoreWebView2Environment* environment) {
        if (!CallbackAlive(alive)) return;
        if (FAILED(result) || !environment || shuttingDown_) {
          creating_ = false;
          creationStartedAt_ = 0;
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
              creationStartedAt_ = 0;
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
        const HRESULT started =
            CreateProfileController(hostWindow_, onController.Get());
        if (FAILED(started)) {
          creating_ = false;
          creationStartedAt_ = 0;
          ScheduleRecreate(L"controller creation could not start " + HResultHex(started));
        }
      });
}

void StationheadPlayer::CompletePendingAuthPopupDeferral() noexcept {
  ComPtr<ICoreWebView2Deferral> deferral = authPopupDeferral_;
  const auto completed = authPopupDeferralCompleted_;
  authPopupDeferral_.Reset();
  authPopupDeferralCompleted_.reset();
  if (deferral && completed &&
      !completed->exchange(true, std::memory_order_acq_rel)) {
    deferral->Complete();
  }
}

void StationheadPlayer::EnsureAuthController(const std::wstring& url) {
  authPendingUrl_ = url;
  if (!environment_ || authController_ || !EnsureAuthHostWindow()) return;
  authCallbackAlive_->store(false, std::memory_order_release);
  authCallbackAlive_ = std::make_shared<std::atomic<bool>>(true);
  const auto alive = authCallbackAlive_;
  authControllerStartedAt_ = UnixMillis();
  const auto onController = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
      [this, alive](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
        if (!CallbackAlive(alive)) {
          if (controller) controller->Close();
          return S_OK;
        }
        authControllerStartedAt_ = 0;
        if (FAILED(result) || !controller || shuttingDown_) {
          if (controller) controller->Close();
          if (!shuttingDown_) {
            FinishSpotifyAuthorization(
                L"Spotify auth controller creation failed " + HResultHex(result));
          }
          return S_OK;
        }
        authController_ = controller;
        authController_->put_IsVisible(FALSE);
        authController_->get_CoreWebView2(&authWebview_);
        if (!authWebview_) {
          FinishSpotifyAuthorization(L"Spotify auth WebView unavailable");
          return S_OK;
        }
        ConfigureAuthWebView();
        return S_OK;
      });
  const HRESULT started =
      CreateProfileController(authHostWindow_, onController.Get());
  if (FAILED(started)) {
    authControllerStartedAt_ = 0;
    authCallbackAlive_->store(false, std::memory_order_release);
    FinishSpotifyAuthorization(
        L"Spotify auth controller could not start " + HResultHex(started));
  }
}

void StationheadPlayer::Tick(int64_t nowMs) {
  if (shuttingDown_) return;
  if (nowMs < nextTickAt_ && !(recreating_.load(std::memory_order_relaxed) && nowMs >= recreateAt_)) {
    return;
  }
  nextTickAt_ = nowMs + 60'000;
  if (creating_.load(std::memory_order_relaxed) && creationStartedAt_ > 0 &&
      nowMs - creationStartedAt_ >= kStationheadWebViewCreationTimeoutMs) {
    createCallbackAlive_->store(false, std::memory_order_release);
    creating_ = false;
    creationStartedAt_ = 0;
    SharedWebViewEnvironment::Instance().Invalidate(userDataFolder_);
    ScheduleRecreate(L"WebView2 environment/controller creation timed out", 1'000);
    nextTickAt_ = nowMs + 1'000;
    return;
  }
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
  if (!startupNavigationStarted_) {
    const bool waitingForAuthCapture =
        !authCaptureScriptRegistrationComplete_;
    const bool waitingForStartupScript =
        !startupScriptRegistrationComplete_;
    if ((waitingForAuthCapture || waitingForStartupScript) &&
        startupScriptDeadline_ > 0 && nowMs >= startupScriptDeadline_) {
      std::wstring missing;
      if (waitingForAuthCapture) missing = L"auth capture";
      if (waitingForStartupScript) {
        if (!missing.empty()) missing += L", ";
        missing += L"startup";
      }
      startupScriptDeadline_ = 0;
      ScheduleRecreate(
          L"startup script registration timed out waiting for " + missing,
          1'000);
      nextTickAt_ = nowMs + 1'000;
      return;
    }
    if (!startupNavigationStarted_) {
      nextTickAt_ = nowMs + 1'000;
      return;
    }
  }
  if (spotifyAuthorization_ && authControllerStartedAt_ > 0 &&
      nowMs - authControllerStartedAt_ >= kStationheadAuthControllerTimeoutMs) {
    authCallbackAlive_->store(false, std::memory_order_release);
    authControllerStartedAt_ = 0;
    CompletePendingAuthPopupDeferral();
    FinishSpotifyAuthorization(L"Spotify auth controller creation timed out");
    nextTickAt_ = nowMs + 1'000;
    return;
  }
  if (!spotifyAuthorization_ && authController_) CloseAuthWebView();
  if (spotifyAuthorization_ || loginRequired_) {
    if (trackBoundaryPlaybackRecoveryPending_) {
      trackBoundaryPlaybackRecoveryPending_ = false;
      trackBoundaryPlaybackRecoveryAwaitingNavigation_ = false;
      trackBoundaryPlaybackRecoveryDeadline_ = 0;
      log_.Info(L"Stationhead " + std::wstring(RoleTag()) +
                L" cancelled track-boundary recovery because interactive authentication is required");
    }
    nextTickAt_ = nowMs + 1'000;
    return;
  }

  bool statusNavigating = false;
  {
    std::lock_guard lock(mutex_);
    statusNavigating = status_.navigating;
  }
  const bool navigationActive =
      navigationInFlight_.load(std::memory_order_acquire) || statusNavigating;

  int64_t next = nowMs + 30 * 60'000;
  const auto consider = [&](int64_t deadline) {
    if (deadline <= nowMs) next = nowMs + 1'000;
    else next = std::min(next, deadline);
  };
  if (trackBoundaryPlaybackRecoveryPending_) {
    if (trackBoundaryPlaybackRecoveryAwaitingNavigation_) {
      if (navigationActive) {
        if (trackBoundaryPlaybackRecoveryDeadline_ > 0 &&
            nowMs >= trackBoundaryPlaybackRecoveryDeadline_) {
          ScheduleRecreate(
              L"track-boundary navigation did not complete before its watchdog deadline",
              2'000);
          return;
        }
        consider(trackBoundaryPlaybackRecoveryDeadline_);
      } else {
        trackBoundaryPlaybackRecoveryAwaitingNavigation_ = false;
        // The atomic flag may still contain an outgoing-document event. Query
        // WebView2 again only after NavigationCompleted released the navigating
        // state, then use that fresh value as the recovery baseline.
        bool playingAfterNavigation = false;
        ComPtr<ICoreWebView2_8> audioView;
        BOOL nativePlaying = FALSE;
        const bool measuredAfterNavigation =
            SUCCEEDED(webview_.As(&audioView)) && audioView &&
            SUCCEEDED(audioView->get_IsDocumentPlayingAudio(&nativePlaying));
        if (measuredAfterNavigation) {
          playingAfterNavigation = nativePlaying != FALSE;
        } else {
          log_.Warn(L"Stationhead " + std::wstring(RoleTag()) +
                    L" could not recheck native audio after track-boundary navigation; waiting for a fresh playback event");
        }
        ApplyAudioPlaybackState(
            playingAfterNavigation, L"post-navigation native confirmation");
        if (!playingAfterNavigation) {
          trackBoundaryPlaybackRecoveryDeadline_ =
              nowMs + kStationheadTrackBoundaryPlaybackRecoveryTimeoutMs;
          log_.Info(L"Stationhead " + std::wstring(RoleTag()) +
                    L" track-boundary navigation completed; armed full 30-second audio recovery window");
          consider(trackBoundaryPlaybackRecoveryDeadline_);
        }
      }
    } else if (trackBoundaryPlaybackRecoveryDeadline_ > 0) {
      if (audioPlaying_.load(std::memory_order_relaxed)) {
        ApplyAudioPlaybackState(true, L"playback recovery confirmation");
      } else if (nowMs >= trackBoundaryPlaybackRecoveryDeadline_) {
        RecoverTrackBoundaryPlayback();
        return;
      } else {
        consider(trackBoundaryPlaybackRecoveryDeadline_);
      }
    }
  }

  // Do not run auth probes, stats scripts, or click scans against a document
  // while WebView2 is replacing it. Window B used to run its local auth probe
  // in this gap, adding avoidable callbacks to the most fragile transition.
  if (navigationActive) {
    nextTickAt_ = nowMs + 1'000;
    return;
  }

  if (!IsSecondary()) {
    if (nowMs - lastDailyPlayStatsAt_ >= kDailyPlayStatsIntervalMs) PollDailyPlayStats(nowMs);
    consider(lastDailyPlayStatsAt_ + kDailyPlayStatsIntervalMs);
  } else {
    if (authProbeInFlight_ && nowMs - authProbeStartedAt_ >= kAuthProbeTimeoutMs) {
      authProbeInFlight_ = false;
      authProbeStartedAt_ = 0;
      log_.Warn(L"Secondary Stationhead auth probe timed out");
    }
    if (lastAuthProbeAt_ == 0 || nowMs - lastAuthProbeAt_ >= kAuthProbeIntervalMs) {
      PollAuthProbe(nowMs);
    }
    if (authProbeInFlight_) consider(authProbeStartedAt_ + kAuthProbeTimeoutMs);
    if (lastAuthProbeAt_ > 0 && !authProbeInFlight_) consider(lastAuthProbeAt_ + kAuthProbeIntervalMs);
  }
  if (!audioPlaying_.load(std::memory_order_relaxed)) {
    if (nowMs >= nextAutoClickAt_) AttemptNativeStartClick(nowMs);
    consider(nextAutoClickAt_);
  }
  nextTickAt_ = std::max(nowMs + 1'000, next);
}

void StationheadPlayer::Reconnect() { ScheduleRecreate(L"manual reconnect"); }

void StationheadPlayer::OpenSpotifyAuthorization(const std::wstring& url) {
  if (url.empty()) return;
  trackBoundaryPlaybackRecoveryPending_ = false;
  trackBoundaryPlaybackRecoveryAwaitingNavigation_ = false;
  trackBoundaryPlaybackRecoveryDeadline_ = 0;
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
  trackBoundaryRefreshPending_ = false;
  spotifyAuthorization_ = false;
  {
    std::lock_guard lock(mutex_);
    status_.navigating = false;
    status_.detail = detail;
  }
  if (webview_ && !stationNavigationStarted_) {
    NavigateCurrentUrl(UnixMillis(), L"post-auth startup");
  }
  SelectTab(StationheadTabKind::None);
  PostChange(StationheadChangeReturnMain | StationheadChangeReleaseAuth);
}

void StationheadPlayer::ShowForLogin() {
  trackBoundaryRefreshPending_ = false;
  trackBoundaryPlaybackRecoveryPending_ = false;
  trackBoundaryPlaybackRecoveryAwaitingNavigation_ = false;
  trackBoundaryPlaybackRecoveryDeadline_ = 0;
  SelectTab(StationheadTabKind::Stationhead);
  log_.Warn(L"Stationhead " + std::wstring(RoleTag()) + L" login required; window visible");
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
  log_.Warn(L"Stationhead " + std::wstring(RoleTag()) + L" audio stopped; restored the player");
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
  audioPlayingSinceAt_.store(0, std::memory_order_relaxed);
  trackBoundaryPlaybackRecoveryPending_ = false;
  trackBoundaryPlaybackRecoveryAwaitingNavigation_ = false;
  trackBoundaryPlaybackRecoveryDeadline_ = 0;
  const int64_t candidate = UnixMillis() + std::max<int64_t>(0, delayMs);
  const bool wasRecreating = recreating_.exchange(true);
  if (!wasRecreating || candidate < recreateAt_) recreateAt_ = candidate;
  nextTickAt_ = 0;
  {
    std::lock_guard lock(mutex_);
    status_.detail = L"recreate scheduled: " + reason;
  }
  log_.Warn(L"Stationhead " + std::wstring(RoleTag()) + L" WebView recreate scheduled: " + reason);
  PostChange();
}

}  // namespace hp
