#pragma comment(lib, "version.lib")

#include "app.h"
#include "cloud_config.h"
#include "version.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {
constexpr wchar_t kWindowClass[] = L"HomePanelNativeWindow";
constexpr UINT_PTR kCentralTimer = 1;
constexpr UINT WM_HP_UPDATE_RESULT = WM_APP + 20;
constexpr int kRestartExitCode = 42;
constexpr uint32_t kFastTickMs = 2000;
constexpr uint32_t kSteadyDashboardTickMs = 5000;
constexpr uint32_t kMaxIdleTickMs = 30'000;
constexpr int64_t kDashboardStartupFallbackMs = 30'000;
int startupShowCommand = SW_SHOW;

constexpr bool DashboardAudioReady(bool primaryAudioReady,
                                   bool secondaryEnabled,
                                   bool secondaryAudioReady) noexcept {
  return primaryAudioReady && (!secondaryEnabled || secondaryAudioReady);
}

static_assert(DashboardAudioReady(true, false, false));
static_assert(!DashboardAudioReady(false, false, false));
static_assert(DashboardAudioReady(true, true, true));
static_assert(!DashboardAudioReady(true, true, false));
static_assert(!DashboardAudioReady(false, true, true));
static_assert(kDashboardStartupFallbackMs == 30'000);

std::wstring InstalledHomePanelVersion(const fs::path& executable) {
  DWORD handle = 0;
  const DWORD size = GetFileVersionInfoSizeW(executable.c_str(), &handle);
  if (!size) return {};
  std::vector<BYTE> data(size);
  if (!GetFileVersionInfoW(executable.c_str(), 0, size, data.data())) return {};
  VS_FIXEDFILEINFO* info = nullptr;
  UINT infoSize = 0;
  if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) ||
      !info || infoSize < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != 0xfeef04bd) {
    return {};
  }
  std::wostringstream version;
  version << HIWORD(info->dwFileVersionMS) << L'.'
          << LOWORD(info->dwFileVersionMS) << L'.'
          << HIWORD(info->dwFileVersionLS);
  const WORD revision = LOWORD(info->dwFileVersionLS);
  if (revision) version << L'.' << revision;
  return version.str();
}


uint32_t NextDelayFromDeadline(int64_t now, int64_t deadline, uint32_t fallbackMs) {
  if (deadline <= 0) return fallbackMs;
  if (deadline <= now) return kFastTickMs;
  const int64_t delta = deadline - now;
  return static_cast<uint32_t>(std::clamp<int64_t>(delta, kFastTickMs, fallbackMs));
}

void EnrichRenderStationheadState(
    StationheadStatus& state,
    StationheadStatus* secondaryStatus,
    const StationheadConfig& config) {
  state.fallbackUrl = config.fallbackUrl;
  if (secondaryStatus) {
    state.loginRequired = state.loginRequired || secondaryStatus->loginRequired;
    state.secondaryAudioMuted = secondaryStatus->audioMuted;
    state.secondaryPlaying = secondaryStatus->playing;
    state.secondaryUrl = std::move(secondaryStatus->url);
  } else {
    state.secondaryAudioMuted = false;
    state.secondaryPlaying = false;
    state.secondaryUrl.clear();
  }
}

}

App::App(HINSTANCE instance) : instance_(instance) { current_ = this; }

App::~App() {
  StopServices();
  if (mutex_) CloseHandle(mutex_);
  current_ = nullptr;
}

App* App::Current() { return current_; }

int App::Run(int showCommand) {
  mutex_ = CreateMutexW(nullptr, TRUE, kMutexName);
  if (!mutex_ || GetLastError() == ERROR_ALREADY_EXISTS) return 0;
  InitializePaths();
  logger_ = std::make_unique<Logger>(dataDir_ / L"homepanel.log", 2 * 1024 * 1024, 3);
  logger_->Info(L"HomePanel starting version " + std::wstring(kVersion));
  CreateMainWindow(showCommand);
  StartServices();
  MSG message{};
  running_ = true;
  int getMessageResult = 0;
  while ((getMessageResult = GetMessageW(&message, nullptr, 0, 0)) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  running_ = false;
  exitCode_ = getMessageResult < 0 ? 1 : static_cast<int>(message.wParam);
  logger_->Info(L"HomePanel exiting code " + std::to_wstring(exitCode_));
  return exitCode_;
}

void App::InitializePaths() {
  wchar_t executable[MAX_PATH * 4]{};
  GetModuleFileNameW(nullptr, executable, _countof(executable));
  rootDir_ = fs::path(executable).parent_path();
  dataDir_ = rootDir_ / L"data";
  fs::create_directories(dataDir_);
  const fs::path settings = dataDir_ / L"settings.json";
  if (!fs::exists(settings) && fs::exists(rootDir_ / L"config.example.json")) {
    std::error_code ignored;
    fs::copy_file(rootDir_ / L"config.example.json", settings, fs::copy_options::skip_existing, ignored);
  }
  config_ = LoadConfig(settings);
  ApplyCloudConfig(config_, dataDir_ / L"device-config.json");
}

void App::CreateMainWindow(int showCommand) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  WNDCLASSEXW windowClass{sizeof(windowClass)};
  windowClass.style = CS_HREDRAW | CS_VREDRAW;
  windowClass.lpfnWndProc = WindowProc;
  windowClass.hInstance = instance_;
  windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  windowClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  windowClass.lpszClassName = kWindowClass;
  if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()), "RegisterClassEx");
  }
  RECT bounds{0, 0, config_.screenWidth, config_.screenHeight};
  HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO info{sizeof(info)};
  if (GetMonitorInfoW(monitor, &info)) bounds = info.rcMonitor;
  window_ = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, kAppName, WS_POPUP | WS_CLIPCHILDREN,
                            bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                            nullptr, nullptr, instance_, this);
  if (!window_) {
    const DWORD primaryError = GetLastError();
    window_ = CreateWindowExW(0, kWindowClass, kAppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, config_.screenWidth, config_.screenHeight,
                              nullptr, nullptr, instance_, this);
    if (window_ && logger_) {
      std::wostringstream text;
      text << L"CreateWindowEx fallback succeeded after popup window failed: " << primaryError;
      logger_->Warn(text.str());
    }
  }
  if (!window_) {
    const DWORD error = GetLastError();
    throw std::runtime_error("CreateWindowEx failed (" + std::to_string(error) + ")");
  }
  startupShowCommand = showCommand == SW_HIDE ? SW_SHOW : showCommand;
}

void App::StartServices() {
  renderer_ = std::make_unique<Renderer>(window_, config_.screenWidth, config_.screenHeight);
  if (!renderer_->LoadDashboard(dataDir_ / L"dashboard.json")) {
    logger_->Warn(L"No valid dashboard cache; local layers will remain available");
  }
  newsCount_ = renderer_->NewsCount();
  newsIndex_ = 0;



  // Keep the primary on its existing folder so its login survives this
  // migration. Window B gets its own user-data folder and therefore its own
  // browser process, cache, service workers, cookies, and storage.
  const fs::path primaryStationheadData = dataDir_ / L"webview2-stationhead";
  const fs::path secondaryStationheadData = dataDir_ / L"webview2-stationhead-secondary";
  stationhead_ = std::make_unique<StationheadPlayer>(
      StationheadRole::Primary, window_, config_.stationhead, primaryStationheadData, *logger_);
  if (config_.stationhead.secondaryEnabled && !config_.stationhead.secondaryUrl.empty()) {
    secondaryStationhead_ = std::make_unique<StationheadPlayer>(
        StationheadRole::Secondary, window_, config_.stationhead, secondaryStationheadData, *logger_);
    logger_->Info(L"Secondary Stationhead prepared with isolated WebView2 user data");
  }
  RECT client{};
  if (GetClientRect(window_, &client) && client.right > client.left && client.bottom > client.top) {
    renderer_->Resize(client.right - client.left, client.bottom - client.top);
    LayoutWorkspace();
  }
  ApplyStartupStationheadPreview();
  startupAt_ = UnixMillis();
  lastNewsRotateAt_ = newsCount_ > 1 ? startupAt_ : 0;

  // Route audio before either WebView can emit its first audio.
  ApplyScheduledStationheadAudioProfile(true);
  stationhead_->Start();
  logger_->Info(L"Primary Stationhead started in the startup preview");
  if (secondaryStationhead_) {
    secondaryStationhead_->Start();
    secondaryStarted_ = true;
    logger_->Info(L"Secondary Stationhead started alongside primary");
  }

  // Do not expose an empty native shell before the Stationhead surfaces
  // have startup geometry and their WebView creation has been requested.
  ShowWindow(window_, startupShowCommand);
  UpdateWindow(window_);
  ScheduleNextTick(kFastTickMs);

  const std::wstring deviceToken = LoadProtectedToken(dataDir_ / L"device-token.dat", L"HOMEPANEL_DEVICE_TOKEN");
  const std::wstring actionToken = LoadProtectedToken(dataDir_ / L"action-token.dat", L"HOMEPANEL_ACTION_TOKEN");
  cloud_ = std::make_unique<CloudClient>(window_, config_, dataDir_, deviceToken, actionToken, *logger_);
  sensors_ = std::make_unique<SensorHub>(window_, config_, dataDir_, *logger_);
  sensors_->Start();

  LoadAirHistory();
  LoadStationheadPlayHistory();
  renderState_.sensors = sensors_->Snapshot();
  UpdateAirHistory(renderState_.sensors);
  renderState_.stationhead = stationhead_->Status();
  renderState_.appVersion = kVersion;
  lastTelemetryAt_ = startupAt_;
  MarkRenderStateDirty();
  InvalidateAll();
}

void App::ApplyStartupStationheadPreview() {
  if (!stationhead_) return;
  RECT bounds = workspaceBounds_;
  if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
    GetClientRect(window_, &bounds);
  }
  if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
    bounds = RECT{0, 0, std::max(1, config_.screenWidth), std::max(1, config_.screenHeight)};
  }

  if (!secondaryStationhead_) {
    stationhead_->SetStartupPreviewBounds(bounds);
    logger_->Info(L"Stationhead startup preview applied full-screen before dashboard");
    return;
  }

  const LONG clientWidth = std::max<LONG>(1, bounds.right - bounds.left);
  const LONG mid = bounds.left + clientWidth / 2;
  RECT left{bounds.left, bounds.top, mid, bounds.bottom};
  RECT right{mid, bounds.top, bounds.right, bounds.bottom};
  if (left.right <= left.left) left.right = left.left + 1;
  if (right.right <= right.left) right.right = right.left + 1;

  stationhead_->SetStartupPreviewBounds(left);
  secondaryStationhead_->SetStartupPreviewBounds(right);
  logger_->Info(L"Stationhead startup preview applied without dashboard gap: A left, B right");
}

void App::ClearStartupStationheadPreview() {
  if (stationhead_) stationhead_->ClearStartupPreviewBounds();
  if (secondaryStationhead_) secondaryStationhead_->ClearStartupPreviewBounds();
}

void App::StartDeferredServices(int64_t now, const StationheadStatus&) {
  const bool primaryAudioReady = stationhead_->RawStatus().audioPlaying;
  const bool secondaryEnabled = static_cast<bool>(secondaryStationhead_);
  bool secondaryAudioReady = true;
  if (secondaryEnabled) {
    const StationheadStatus secondaryStatus = secondaryStationhead_->RawStatus();
    secondaryAudioReady = secondaryStatus.audioPlaying;
  }
  const bool dashboardAudioReady =
      DashboardAudioReady(primaryAudioReady, secondaryEnabled, secondaryAudioReady);
  const bool startupDeadlineReached = now - startupAt_ >= kDashboardStartupFallbackMs;
  if (dashboardAudioReady && playbackReadyAt_ == 0) playbackReadyAt_ = now;

  if (!rendererStarted_ && (dashboardAudioReady || startupDeadlineReached)) {
    renderer_->Initialize();
    rendererStarted_ = true;
    ClearStartupStationheadPreview();
    LayoutWorkspace();
    PublishRenderStateNow();
    renderer_->TickNativePanels(now);
    InvalidateAll();
    if (dashboardAudioReady) {
      logger_->Info(secondaryEnabled
          ? L"Native dashboard started after Stationhead A/B audio confirmation"
          : L"Native dashboard started after Stationhead audio confirmation");
    } else {
      logger_->Info(L"Native dashboard started after startup fallback deadline");
    }
  }

  if (!cloudStarted_ && (dashboardAudioReady || startupDeadlineReached)) {
    cloud_->Start();
    cloudStarted_ = true;
    logger_->Info(dashboardAudioReady
        ? L"Cloud synchronization started after Stationhead startup confirmation"
        : L"Cloud synchronization started after startup fallback deadline");
  }

  const bool updateDelayElapsed = playbackReadyAt_ > 0
      ? now - playbackReadyAt_ >= 15'000
      : now - startupAt_ >= 60'000;
  if (!startupUpdateScheduled_ && cloudStarted_ && updateDelayElapsed) {
    startupUpdateScheduled_ = true;
    CheckForUpdateAsync(false);
    logger_->Info(L"Background update check started after startup-critical work");
  }
}

void App::StopServices() {
  if (window_) KillTimer(window_, kCentralTimer);
  if (secondaryStationhead_) secondaryStationhead_->Stop();
  if (stationhead_) stationhead_->Stop();
  if (cloud_) cloud_->Stop();
  if (sensors_) sensors_->Stop();
  if (telemetryThread_.joinable()) telemetryThread_.join();
  if (updateThread_.joinable()) updateThread_.join();
  secondaryStationhead_.reset();
  stationhead_.reset();
  cloud_.reset();
  sensors_.reset();
  renderer_.reset();
}

void App::UpdateStationheadPlaybackFallback(int64_t nowMs) {
  if (!rendererStarted_ || !renderer_ || !stationhead_) return;
  const NativePlaybackFeedStatus feed =
      renderer_->NativePlaybackFeedStatusFor(0, nowMs);
  const bool noNextTrack = feed.endedWithoutNextTrack && feed.contentRevision != 0;

  if (stationheadPlaybackFallbackActive_) {
    if (feed.contentRevision > stationheadPlaybackFallbackRevision_) {
      stationheadPlaybackFallbackActive_ = false;
      stationheadPlaybackFallbackRevision_ = 0;
      stationhead_->SetPlaybackFallback(
          false, L"new playback-a information; returning to primary URL");
      if (secondaryStationhead_) {
        secondaryStationhead_->SetPlaybackFallback(
            false, L"new playback-a information; returning to secondary URL");
      }
      logger_->Info(L"Stationhead playback-a updated; returning both windows from fallback");
    }
    stationheadPlaybackNoNextTrackObserved_ = noNextTrack;
    return;
  }

  if (noNextTrack && !stationheadPlaybackNoNextTrackObserved_ &&
      !config_.stationhead.fallbackUrl.empty()) {
    stationheadPlaybackFallbackActive_ = true;
    stationheadPlaybackFallbackRevision_ = feed.contentRevision;
    stationhead_->SetPlaybackFallback(
        true, L"playback-a has no new next-track information; switching to fallback");
    if (secondaryStationhead_) {
      secondaryStationhead_->SetPlaybackFallback(
          true, L"playback-a has no new next-track information; switching to fallback");
    }
    logger_->Warn(L"Stationhead playback-a reached the end of known tracks; switching both windows to fallback");
  }
  stationheadPlaybackNoNextTrackObserved_ = noNextTrack;
}

void App::Tick() {
  if (!renderer_ || !sensors_ || !stationhead_ || !cloud_) return;
  const int64_t now = UnixMillis();

  if (secondaryStarted_ && secondaryStationhead_) secondaryStationhead_->Tick(now);
  StationheadStatus secondaryStatus =
      secondaryStationhead_ ? secondaryStationhead_->Status() : StationheadStatus{};
  stationhead_->Tick(now);
  StationheadStatus nextStationheadState = stationhead_->Status();
  EnrichRenderStationheadState(
      nextStationheadState,
      secondaryStationhead_ ? &secondaryStatus : nullptr,
      config_.stationhead);
  nextStationheadState.primaryAudioSelected = scheduledPrimaryAudioAudible_;
  UpdateRenderStationheadState(std::move(nextStationheadState));
  const StationheadStatus& stationheadStatus = renderState_.stationhead;
  UpdateStationheadPlayHistory(stationheadStatus);
  StartDeferredServices(now, stationheadStatus);
  ApplyStationheadWindowPlacement(stationheadStatus, secondaryStatus);

  if (cloudStarted_ &&
      now - lastTelemetryAt_ >= static_cast<int64_t>(config_.telemetryMinutes) * 60'000) {
    lastTelemetryAt_ = now;
    SendTelemetryAsync();
  }
  if (toastUntil_ && now >= toastUntil_) {
    toastUntil_ = 0;
    renderState_.toast.clear();
    PublishRenderStateNow();
  }
  if (newsCount_ > 1 && lastNewsRotateAt_ > 0 && now - lastNewsRotateAt_ >= 30'000) {
    lastNewsRotateAt_ = now;
    newsIndex_ = (newsIndex_ + 1) % newsCount_;
    renderState_.newsIndex = newsIndex_;
    PublishRenderStateNow();
  }
  PublishRenderState();
  if (rendererStarted_) renderer_->TickNativePanels(now);
  UpdateStationheadPlaybackFallback(now);
  uint32_t nextTickMs = kMaxIdleTickMs;
  const bool stationheadNeedsFastTick =
      !rendererStarted_ || renderState_.maintenance ||
      StationheadNeedsForeground(stationheadStatus) ||
      (secondaryStationhead_ && StationheadNeedsForeground(secondaryStatus));
  if (stationheadNeedsFastTick) {
    nextTickMs = kFastTickMs;
  } else if (selectedTab_ == WorkspaceTab::Main) {
    nextTickMs = kSteadyDashboardTickMs;
  } else {
    nextTickMs = std::min(nextTickMs, NextDelayFromDeadline(now, stationhead_->NextWakeAt(), kMaxIdleTickMs));
    if (secondaryStarted_ && secondaryStationhead_) {
      nextTickMs = std::min(nextTickMs, NextDelayFromDeadline(now, secondaryStationhead_->NextWakeAt(), kMaxIdleTickMs));
    }
    if (toastUntil_ > 0) nextTickMs = std::min(nextTickMs, NextDelayFromDeadline(now, toastUntil_, kMaxIdleTickMs));
    if (newsCount_ > 1 && lastNewsRotateAt_ > 0) {
      nextTickMs = std::min(nextTickMs, NextDelayFromDeadline(now, lastNewsRotateAt_ + 30'000, kMaxIdleTickMs));
    }
  }
  ScheduleNextTick(nextTickMs);
}

void App::Draw() {
  PAINTSTRUCT paint{};
  BeginPaint(window_, &paint);
  if (renderer_ && rendererStarted_) {
    PublishRenderState();
    // Message handlers own state updates. Avoid sensor locks and Stationhead status
    // reconstruction on every incidental WM_PAINT.
    renderer_->Render(paint.rcPaint, renderState_);
  }
  EndPaint(window_, &paint);
}

void App::ShowToast(std::wstring message, int64_t durationMs, bool invalidate) {
  renderState_.toast = std::move(message);
  toastUntil_ = durationMs > 0 ? UnixMillis() + durationMs : 0;
  if (invalidate) PublishRenderStateNow();
  else MarkRenderStateDirty();
}

bool App::UpdateRenderStationheadState(StationheadStatus nextState) {
  if (renderState_.stationhead == nextState) return false;
  renderState_.stationhead = std::move(nextState);
  MarkRenderStateDirty();
  return true;
}

void App::LayoutWorkspace() {
  if (!renderer_ || !stationhead_) return;
  RECT client{};
  GetClientRect(window_, &client);
  workspaceBounds_ = client;
  renderer_->SetBounds(workspaceBounds_);
  renderer_->SetVisible(rendererStarted_ && selectedTab_ == WorkspaceTab::Main);

  const int clientWidth = std::max(1L, client.right - client.left);
  const int clientHeight = std::max(1L, client.bottom - client.top);
  const RECT fullBounds{client.left, client.top, client.left + clientWidth, client.top + clientHeight};

  switch (selectedTab_) {
    case WorkspaceTab::Main:
      MarkStationheadPlacementDirty();
      ApplyStationheadWindowPlacement(stationhead_->Status(),
          secondaryStationhead_ ? secondaryStationhead_->Status() : StationheadStatus{});
      break;
    case WorkspaceTab::Stationhead:
      stationhead_->SetBounds(fullBounds);
      if (secondaryStationhead_) secondaryStationhead_->SetBounds(fullBounds);
      stationhead_->SelectTab(StationheadTabKind::Stationhead);
      break;
    case WorkspaceTab::Auth:
      stationhead_->SetBounds(fullBounds);
      if (secondaryStationhead_) secondaryStationhead_->SetBounds(fullBounds);
      if (stationhead_->HasAuthTab()) {
        stationhead_->SelectTab(StationheadTabKind::Auth);
      } else {
        selectedTab_ = WorkspaceTab::Main;
        stationhead_->SelectTab(StationheadTabKind::None);
      }
      break;
  }
  renderState_.workspaceTab = static_cast<int>(selectedTab_);
  MarkRenderStateDirty();
  InvalidateAll();
}

void App::ApplyStationheadWindowPlacement(const StationheadStatus& primaryStatus,
                                          const StationheadStatus& secondaryStatus) {
  if (!rendererStarted_ || !stationhead_ || selectedTab_ != WorkspaceTab::Main) return;
  RECT bounds = workspaceBounds_;
  if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;

  const bool primaryPending = !primaryStatus.audioPlaying;
  const bool secondaryPending = secondaryStationhead_ && !secondaryStatus.playing;
  // This runs on every tick. Re-apply the placement only when the pending
  // state or geometry changed, or a player posted a state change (which can
  // reposition its own windows, e.g. a reload's SetStartupBounds) - steady

  if (!stationheadPlacementDirty_ && primaryPending == placedPrimaryPending_ &&
      secondaryPending == placedSecondaryPending_ && EqualRect(&bounds, &placedBounds_)) {
    return;
  }
  stationheadPlacementDirty_ = false;
  placedPrimaryPending_ = primaryPending;
  placedSecondaryPending_ = secondaryPending;
  placedBounds_ = bounds;

  const LONG mid = bounds.left + std::max<LONG>(1, bounds.right - bounds.left) / 2;
  RECT left{bounds.left, bounds.top, mid, bounds.bottom};
  RECT right{mid, bounds.top, bounds.right, bounds.bottom};
  if (left.right <= left.left) left.right = left.left + 1;
  if (right.right <= right.left) right.right = right.left + 1;

  stationhead_->SetBounds(primaryPending ? left : bounds);
  stationhead_->SelectTab(StationheadTabKind::None);
  if (secondaryStationhead_) {
    secondaryStationhead_->SetBounds(secondaryPending ? right : bounds);
    secondaryStationhead_->RefreshVisibility();
  }
}

void App::PublishRenderState() {
  if (!renderer_ || !renderStateDirty_) return;
  renderer_->UpdateState(renderState_);
  renderStateDirty_ = false;
}

void App::PublishRenderStateNow() {
  MarkRenderStateDirty();
  if (!rendererStarted_) return;
  PublishRenderState();
}

void App::ApplyScheduledStationheadAudioProfile(bool primaryAudible) noexcept {
  scheduledPrimaryAudioAudible_ = primaryAudible;
  const bool primaryMuted = stationheadAudioMuted_ || !primaryAudible;
  const bool secondaryMuted = stationheadAudioMuted_ || primaryAudible;
  if (stationhead_) stationhead_->SetAudioMuted(primaryMuted);
  if (secondaryStationhead_) secondaryStationhead_->SetAudioMuted(secondaryMuted);
  if (renderState_.stationhead.audioMuted != primaryMuted ||
      renderState_.stationhead.secondaryAudioMuted != secondaryMuted ||
      renderState_.stationhead.primaryAudioSelected != primaryAudible) {
    renderState_.stationhead.audioMuted = primaryMuted;
    renderState_.stationhead.secondaryAudioMuted = secondaryMuted;
    renderState_.stationhead.primaryAudioSelected = primaryAudible;
    MarkRenderStateDirty();
  }
}

void App::ScheduleNextTick(uint32_t milliseconds) {
  if (!window_) return;
  const uint32_t clamped = std::max<uint32_t>(1, milliseconds);
  if (nextAppTickAt_ == static_cast<int64_t>(clamped)) return;
  KillTimer(window_, kCentralTimer);
  SetTimer(window_, kCentralTimer, clamped, nullptr);
  nextAppTickAt_ = clamped;
}

void App::InvalidateAll() {
  ::InvalidateRect(window_, nullptr, FALSE);
}


void App::HandleAction(UiAction action) {
  switch (action) {
    case UiAction::WorkspaceMain:
      selectedTab_ = WorkspaceTab::Main;
      LayoutWorkspace();
      break;
    case UiAction::WorkspaceAuth:
      if (stationhead_->HasAuthTab()) {
        selectedTab_ = WorkspaceTab::Auth;
        LayoutWorkspace();
      } else {
        renderState_.toast = L"認証タブが開いていません";
        toastUntil_ = UnixMillis() + 3000;
        PublishRenderStateNow();
      }
      break;
    case UiAction::DataRefresh:
      renderState_.toast = cloud_->RequestRemoteRefresh()
        ? L"Cloudflareへ更新を要求しました" : L"更新要求に失敗しました";
      toastUntil_ = UnixMillis() + 4000;
      PublishRenderStateNow();
      break;
    case UiAction::AppUpdate:
      CheckForUpdateAsync(true);
      break;
    case UiAction::Restart:
      exitCode_ = kRestartExitCode;
      DestroyWindow(window_);
      break;
    case UiAction::Maintenance:
      renderState_.maintenance = !renderState_.maintenance;
      PublishRenderStateNow();
      break;
    case UiAction::StationheadReconnect:
      stationhead_->Reconnect();
      break;
    case UiAction::StationheadAudioToggle:
      ToggleStationheadAudio();
      break;
    case UiAction::StationheadAudioMute:
      MuteStationheadAudio();
      break;
    case UiAction::ClearCache:
      ClearDisplayCache();
      break;
    case UiAction::ShowLog:
      ShellExecuteW(window_, L"open", L"notepad.exe", QuotePath(dataDir_ / L"homepanel.log").c_str(),
                    rootDir_.c_str(), SW_SHOWNORMAL);
      break;
    case UiAction::CloseMaintenance:
      renderState_.maintenance = false;
      PublishRenderStateNow();
      break;
    default:
      break;
  }
}

void App::LogUnhandled(DWORD code, void* address) {
  if (logger_) {
    std::wostringstream text;
    text << L"Unhandled exception 0x" << std::hex << code << L" at " << address;
    logger_->Error(text.str());
  }
}
}




#include "app_air_history.cpp"
#include "app_stationhead_history.cpp"
#include "app_update.cpp"
#include "app_messages.cpp"
#include "app_commands.cpp"
