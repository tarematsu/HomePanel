from pathlib import Path
from textwrap import dedent


def block(value: str) -> str:
    return dedent(value).lstrip("\n")


def replace_once(path: str, old: str, new: str) -> None:
    file = Path(path)
    text = file.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one match, found {count}")
    file.write_text(text.replace(old, new, 1), encoding="utf-8")


replace_once(
    "native/src/sh.h",
    block(
        """
          void ApplyAudioPlaybackState(bool playing, const std::wstring& source);
          void HandleTrackEnded(int64_t nowMs, bool retry);
          void TryStartInitialNavigation();
        """
    ),
    block(
        """
          void ApplyAudioPlaybackState(bool playing, const std::wstring& source);
          void HandleTrackEnded(int64_t nowMs, bool retry);
          void RecoverTrackBoundaryPlayback();
          void TryStartInitialNavigation();
        """
    ),
)
replace_once(
    "native/src/sh.h",
    block(
        """
          std::atomic<bool> navigationInFlight_{false};
          bool trackBoundaryRefreshPending_ = false;
          int64_t creationStartedAt_ = 0;
        """
    ),
    block(
        """
          std::atomic<bool> navigationInFlight_{false};
          bool trackBoundaryRefreshPending_ = false;
          bool trackBoundaryPlaybackRecoveryPending_ = false;
          int64_t trackBoundaryPlaybackRecoveryDeadline_ = 0;
          int64_t creationStartedAt_ = 0;
        """
    ),
)

replace_once(
    "native/src/sh.cpp",
    "constexpr int64_t kStationheadTrackBoundaryRefreshDelayMs = 52 * 60'000;\n",
    "constexpr int64_t kStationheadTrackBoundaryRefreshDelayMs = 52 * 60'000;\n"
    "constexpr int64_t kStationheadTrackBoundaryPlaybackRecoveryTimeoutMs = 30'000;\n",
)
replace_once(
    "native/src/sh.cpp",
    block(
        """
          if (playing) {
            resourceBlockingArmed_ = true;
        """
    ),
    block(
        """
          if (playing) {
            if (trackBoundaryPlaybackRecoveryPending_) {
              trackBoundaryPlaybackRecoveryPending_ = false;
              trackBoundaryPlaybackRecoveryDeadline_ = 0;
              log_.Info(L"Stationhead " + std::wstring(RoleTag()) +
                        L" audio recovered after track-boundary refresh");
            }
            resourceBlockingArmed_ = true;
        """
    ),
)
replace_once(
    "native/src/sh.cpp",
    block(
        """
          trackBoundaryRefreshPending_ = false;
          lastReloadAt_ = nowMs;
          NavigateCurrentUrl(nowMs, L"track-boundary authentication refresh");
        }

        void StationheadPlayer::TryStartInitialNavigation() {
        """
    ),
    block(
        r"""
          trackBoundaryRefreshPending_ = false;
          lastReloadAt_ = nowMs;
          NavigateCurrentUrl(nowMs, L"track-boundary authentication refresh");
        }

        void StationheadPlayer::RecoverTrackBoundaryPlayback() {
          if (!trackBoundaryPlaybackRecoveryPending_ || !webview_ ||
              audioPlaying_.load(std::memory_order_relaxed) || spotifyAuthorization_ ||
              loginRequired_ || recreating_.load(std::memory_order_relaxed) ||
              navigationInFlight_.load(std::memory_order_acquire)) {
            return;
          }

          trackBoundaryPlaybackRecoveryPending_ = false;
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
          const media = Array.from(document.querySelectorAll('audio,video')).map((element, index) => ({
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
              message: String(element.error.message || ''),
            } : null,
            sourceHost: sourceHost(element),
          }));
          return {
            locationHost: String(location.hostname || ''),
            locationPath: String(location.pathname || ''),
            visibility: document.visibilityState,
            focused: document.hasFocus(),
            mediaSession: navigator.mediaSession?.playbackState || '',
            audioFlag: window.__homepanelAudioPlaying,
            media,
          };
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
                    if (diagnostic.size() > 2'048) diagnostic.resize(2'048);
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
        """
    ),
)
replace_once(
    "native/src/sh.cpp",
    block(
        """
        void StationheadPlayer::NavigateStationheadUrl(int64_t nowMs, const std::wstring& url,
                                                       const std::wstring& reason,
                                                       bool fallbackActive) {
          (void)nowMs;
          if (!webview_ || url.empty()) return;
          stationNavigationStarted_ = true;
          SetStartupBounds();
          ResetNavigationRouteState();
        """
    ),
    block(
        """
        void StationheadPlayer::NavigateStationheadUrl(int64_t nowMs, const std::wstring& url,
                                                       const std::wstring& reason,
                                                       bool fallbackActive) {
          if (!webview_ || url.empty()) return;
          const bool trackBoundaryRefresh =
              reason == L"track-boundary authentication refresh";
          trackBoundaryPlaybackRecoveryPending_ = trackBoundaryRefresh;
          trackBoundaryPlaybackRecoveryDeadline_ = trackBoundaryRefresh
              ? nowMs + kStationheadTrackBoundaryPlaybackRecoveryTimeoutMs
              : 0;
          stationNavigationStarted_ = true;
          SetStartupBounds();
          ResetNavigationRouteState();
        """
    ),
)
replace_once(
    "native/src/sh.cpp",
    block(
        """
          const HRESULT result = webview_->Navigate(url.c_str());
          if (FAILED(result)) {
            ScheduleRecreate(L"navigation start failed " + HResultHex(result), 1'000);
            return;
          }
          log_.Info(L"Stationhead " + std::wstring(RoleTag()) + L" navigation (" + reason + L"): " + url);
        """
    ),
    block(
        """
          const HRESULT result = webview_->Navigate(url.c_str());
          if (FAILED(result)) {
            trackBoundaryPlaybackRecoveryPending_ = false;
            trackBoundaryPlaybackRecoveryDeadline_ = 0;
            ScheduleRecreate(L"navigation start failed " + HResultHex(result), 1'000);
            return;
          }
          if (trackBoundaryRefresh) {
            log_.Info(L"Stationhead " + std::wstring(RoleTag()) +
                      L" armed 30-second playback recovery after track-boundary refresh");
          }
          log_.Info(L"Stationhead " + std::wstring(RoleTag()) + L" navigation (" + reason + L"): " + url);
        """
    ),
)
replace_once(
    "native/src/sh.cpp",
    block(
        """
          const auto consider = [&](int64_t deadline) {
            if (deadline <= nowMs) next = nowMs + 1'000;
            else next = std::min(next, deadline);
          };
          if (!IsSecondary()) {
        """
    ),
    block(
        """
          const auto consider = [&](int64_t deadline) {
            if (deadline <= nowMs) next = nowMs + 1'000;
            else next = std::min(next, deadline);
          };
          if (trackBoundaryPlaybackRecoveryPending_ &&
              trackBoundaryPlaybackRecoveryDeadline_ > 0) {
            bool statusNavigating = false;
            {
              std::lock_guard lock(mutex_);
              statusNavigating = status_.navigating;
            }
            if (nowMs >= trackBoundaryPlaybackRecoveryDeadline_ &&
                !navigationInFlight_.load(std::memory_order_acquire) &&
                !statusNavigating) {
              RecoverTrackBoundaryPlayback();
              return;
            }
            consider(trackBoundaryPlaybackRecoveryDeadline_);
          }
          if (!IsSecondary()) {
        """
    ),
)
replace_once(
    "native/src/sh.cpp",
    block(
        """
        void StationheadPlayer::ScheduleRecreate(const std::wstring& reason, int64_t delayMs) {
          if (shuttingDown_) return;
          const int64_t candidate = UnixMillis() + std::max<int64_t>(0, delayMs);
        """
    ),
    block(
        """
        void StationheadPlayer::ScheduleRecreate(const std::wstring& reason, int64_t delayMs) {
          if (shuttingDown_) return;
          trackBoundaryPlaybackRecoveryPending_ = false;
          trackBoundaryPlaybackRecoveryDeadline_ = 0;
          const int64_t candidate = UnixMillis() + std::max<int64_t>(0, delayMs);
        """
    ),
)

replace_once(
    "native/src/app_stationhead_handles.h",
    "inline constexpr int64_t kStationheadTrackTransitionGraceMs = 12'000;",
    "inline constexpr int64_t kStationheadTrackTransitionGraceMs = 30'000;",
)
replace_once(
    "native/src/shared_webview_environment.cpp",
    "HardwareSecureDecryption,HardwareSecureDecryptionExperiment,HardwareSecureDecryptionFallback",
    "HardwareSecureDecryption,HardwareSecureDecryptionExperiment",
)

for path, token in (
    ("native/src/sh.cpp", "RecoverTrackBoundaryPlayback"),
    ("native/src/sh.h", "trackBoundaryPlaybackRecoveryDeadline_"),
    ("native/src/app_stationhead_handles.h", "kStationheadTrackTransitionGraceMs = 30'000"),
):
    if token not in Path(path).read_text(encoding="utf-8"):
        raise RuntimeError(f"{path}: validation token missing: {token}")
if "HardwareSecureDecryptionFallback" in Path(
    "native/src/shared_webview_environment.cpp"
).read_text(encoding="utf-8"):
    raise RuntimeError("HardwareSecureDecryptionFallback is still disabled")

Path(".github/workflows/agent-stationhead-recovery-patch.yml").unlink()
Path(__file__).unlink()
