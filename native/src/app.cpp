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
constexpr uint32_t kFastTickMs = 1000;
constexpr uint32_t kIdleTickMs = 5000;
constexpr uint32_t kMaxIdleTickMs = 30'000;

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

bool SameStationheadStatus(const StationheadStatus& left, const StationheadStatus& right) {
  return left.created == right.created &&
         left.navigating == right.navigating &&
         left.playing == right.playing &&
         left.loginRequired == right.loginRequired &&
         left.spotifyAuthorization == right.spotifyAuthorization &&
         left.apiAuthorization == right.apiAuthorization &&
         left.lightweight == right.lightweight &&
         left.visible == right.visible &&
         left.processFailed == right.processFailed &&
         left.spotifyConfigured == right.spotifyConfigured &&
         left.authAvailable == right.authAvailable &&
         left.audioPlaying == right.audioPlaying &&
         left.audioSilent == right.audioSilent &&
         left.audioMuted == right.audioMuted &&
         left.secondaryAudioMuted == right.secondaryAudioMuted &&
         left.healthMisses == right.healthMisses &&
         left.lastPlaybackConfirmedAt == right.lastPlaybackConfirmedAt &&
         left.processWorkingSet == right.processWorkingSet &&
         left.processCpuPercent == right.processCpuPercent &&
         left.blockedResources == right.blockedResources &&
         left.url == right.url &&
         left.detail == right.detail &&
         left.trackTitle == right.trackTitle &&
         left.trackArtist == right.trackArtist &&
         left.deviceName == right.deviceName &&
         left.artworkUrl == right.artworkUrl &&
         left.sampledAt == right.sampledAt &&
         left.expectedEndAt == right.expectedEndAt &&
         left.trackDurationMs == right.trackDurationMs;
}

uint32_t NextDelayFromDeadline(int64_t now, int64_t deadline, uint32_t fallbackMs) {
  if (deadline <= 0) return fallbackMs;
  if (deadline <= now) return kFastTickMs;
  const int64_t delta = deadline - now;
  return static_cast<uint32_t>(std::clamp<int64_t>(delta, kFastTickMs, fallbackMs));
}

StationheadStatus BuildRenderStationheadState(const AppStationheadHandle& stationhead,
                                              const AppSecondaryStationheadHandle& secondary) {
  StationheadStatus state = stationhead.Status();
  state.secondaryAudioMuted = static_cast<bool>(secondary) && secondary.AudioMuted();
  return state;
}

}  // namespace

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
  if (!window_) ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowEx");
  ShowWindow(window_, showCommand == SW_HIDE ? SW_SHOW : showCommand);
  UpdateWindow(window_);
  ScheduleNextTick(kFastTickMs);
}

void App::StartServices() {
  startupAt_ = UnixMillis();
  radar_ = std::make_unique<RadarManager>(dataDir_, *logger_);
  renderer_ = std::make_unique<Renderer>(window_, config_.screenWidth, config_.screenHeight, *radar_);
  if (!renderer_->LoadDashboard(dataDir_ / L"dashboard.json")) {
    logger_->Warn(L"No valid dashboard cache; local layers will remain available");
  }
  newsCount_ = renderer_->NewsCount();
  newsIndex_ = 0;
  lastNewsRotateAt_ = newsCount_ > 1 ? startupAt_ : 0;

  // Stationhead owns the first shared WebView2 environment/controller so its heavy
  // page bootstrap is not queued behind the dashboard renderer.
  const fs::path stationheadData = dataDir_ / L"webview2-stationhead";
  stationhead_ = std::make_unique<StationheadPlayer>(
      window_, config_.stationhead, stationheadData, *logger_);
  if (config_.stationhead.secondaryEnabled && !config_.stationhead.secondaryUrl.empty()) {
    secondaryStationhead_ = std::make_unique<SecondaryStationheadPlayer>(
        window_, config_.stationhead, stationheadData, *logger_);
    logger_->Info(L"Secondary Stationhead prepared to start alongside primary");
  }
  RECT client{};
  if (GetClientRect(window_, &client) && client.right > client.left && client.bottom > client.top) {
    renderer_->Resize(client.right - client.left, client.bottom - client.top);
    LayoutWorkspace();
  }
  stationhead_->Start();
  logger_->Info(L"Stationhead startup was prioritized before native dashboard initialization");
  // Windows A and B are started together and then temporarily shown as a split
  // preview, so startup order is visually deterministic: A/B first, dashboard next.
  if (secondaryStationhead_) {
    secondaryStationhead_->Start();
    secondaryStarted_ = true;
    logger_->Info(L"Secondary Stationhead started alongside primary");
  }
  ApplyStartupStationheadPreview();
  ApplyScheduledStationheadAudioProfile(true);

  const std::wstring deviceToken = LoadProtectedToken(dataDir_ / L"device-token.dat", L"HOMEPANEL_DEVICE_TOKEN");
  const std::wstring actionToken = LoadProtectedToken(dataDir_ / L"action-token.dat", L"HOMEPANEL_ACTION_TOKEN");
  cloud_ = std::make_unique<CloudClient>(window_, config_, dataDir_, deviceToken, actionToken, *logger_);
  sensors_ = std::make_unique<SensorHub>(window_, config_, dataDir_, *logger_);
  sensors_->Start();

  LoadAirHistory();
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
  const LONG gap = std::clamp<LONG>(clientWidth / 100, 8, 24);
  const LONG mid = bounds.left + clientWidth / 2;
  const LONG halfGap = std::min<LONG>(gap / 2, std::max<LONG>(0, clientWidth / 2 - 1));
  RECT left{bounds.left, bounds.top, mid - halfGap, bounds.bottom};
  RECT right{mid + halfGap, bounds.top, bounds.right, bounds.bottom};
  if (left.right <= left.left) left.right = left.left + 1;
  if (right.right <= right.left) right.right = right.left + 1;

  stationhead_->SetStartupPreviewBounds(left);
  secondaryStationhead_->SetStartupPreviewBounds(right);
  logger_->Info(L"Stationhead startup preview applied: A left, B right");
}

void App::ClearStartupStationheadPreview() {
  if (stationhead_) stationhead_->ClearStartupPreviewBounds();
  if (secondaryStationhead_) secondaryStationhead_->ClearStartupPreviewBounds();
}

void App::StartDeferredServices(int64_t now, const StationheadStatus& stationheadStatus) {
  const bool primaryAudioReady = stationheadStatus.audioPlaying || stationheadStatus.lightweight;
  // The split A/B startup preview stays full-size and in front (so both windows
  // finish loading and the auto-play scan has real geometry) until playback is
  // confirmed. Only then does the native dashboard take over and both
  // Stationhead windows drop to the background. Read the secondary status once,
  // and only while the dashboard has not started yet.
  bool secondaryAudioReady = true;
  bool secondaryLoginRequired = false;
  if (!rendererStarted_ && secondaryStationhead_) {
    const SecondaryStationheadStatus secondaryStatus = secondaryStationhead_->Status();
    secondaryAudioReady = secondaryStatus.playing;
    secondaryLoginRequired = secondaryStatus.loginRequired;
  }
  const bool dashboardAudioReady = primaryAudioReady && secondaryAudioReady;
  // A login prompt on either window can never auto-confirm audio, so let it hand
  // the screen to the dashboard immediately (the prompt itself stays in front).
  const bool loginRequired = stationheadStatus.loginRequired || secondaryLoginRequired;
  const bool startupDeadlineReached = now - startupAt_ >= 30'000;
  if (primaryAudioReady && playbackReadyAt_ == 0) playbackReadyAt_ = now;

  if (!rendererStarted_ &&
      (dashboardAudioReady || startupDeadlineReached || loginRequired)) {
    renderer_->Initialize();
    rendererStarted_ = true;
    ClearStartupStationheadPreview();
    LayoutWorkspace();
    PublishRenderStateNow();
    renderer_->TickNativePanels(now);
    InvalidateAll();
    logger_->Info(dashboardAudioReady
        ? L"Native dashboard started after Stationhead A/B audio confirmation"
        : (loginRequired
            ? L"Native dashboard started because Stationhead login is required"
            : L"Native dashboard started after startup fallback deadline"));
  }

  if (!cloudStarted_ && (primaryAudioReady || startupDeadlineReached)) {
    cloud_->Start();
    cloudStarted_ = true;
    logger_->Info(primaryAudioReady
        ? L"Cloud synchronization started after Stationhead audio confirmation"
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
  radar_.reset();
}

void App::Tick() {
  if (!renderer_ || !sensors_ || !stationhead_ || !radar_ || !cloud_) return;
  const int64_t now = UnixMillis();

  stationhead_->Tick(now);
  if (secondaryStarted_ && secondaryStationhead_) secondaryStationhead_->Tick(now);
  const StationheadStatus stationheadStatus = stationhead_->Status();
  StationheadStatus nextStationheadState = BuildRenderStationheadState(stationhead_, secondaryStationhead_);
  UpdateRenderStationheadState(nextStationheadState);
  StartDeferredServices(now, stationheadStatus);

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
  uint32_t nextTickMs = kMaxIdleTickMs;
  if (!rendererStarted_ || selectedTab_ == WorkspaceTab::Main ||
      renderState_.maintenance || StationheadNeedsForeground(renderState_.stationhead)) {
    nextTickMs = kFastTickMs;
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
  if (renderer_) {
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

bool App::UpdateRenderStationheadState(const StationheadStatus& nextState) {
  if (SameStationheadStatus(renderState_.stationhead, nextState)) return false;
  renderState_.stationhead = nextState;
  MarkRenderStateDirty();
  return true;
}

void App::LayoutWorkspace() {
  if (!renderer_ || !stationhead_) return;
  RECT client{};
  GetClientRect(window_, &client);
  workspaceBounds_ = client;
  renderer_->SetBounds(workspaceBounds_);
  renderer_->SetVisible(selectedTab_ == WorkspaceTab::Main);

  const int clientWidth = std::max(1L, client.right - client.left);
  const int clientHeight = std::max(1L, client.bottom - client.top);
  const RECT fullBounds{client.left, client.top, client.left + clientWidth, client.top + clientHeight};
  stationhead_->SetBounds(fullBounds);
  if (secondaryStationhead_) {
    secondaryStationhead_->SetBounds(fullBounds);
  }


  switch (selectedTab_) {
    case WorkspaceTab::Main:
      stationhead_->SelectTab(StationheadTabKind::None);
      break;
    case WorkspaceTab::Stationhead:
      stationhead_->SelectTab(StationheadTabKind::Stationhead);
      break;
    case WorkspaceTab::Auth:
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
  if (stationhead_) stationhead_->SetAudioMuted(!primaryAudible);
  if (secondaryStationhead_) secondaryStationhead_->SetAudioMuted(primaryAudible);
  if (renderState_.stationhead.audioMuted != !primaryAudible ||
      renderState_.stationhead.secondaryAudioMuted != primaryAudible) {
    renderState_.stationhead.audioMuted = !primaryAudible;
    renderState_.stationhead.secondaryAudioMuted = primaryAudible;
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

void App::Invalidate(const RECT& rect) {
  ::InvalidateRect(window_, &rect, FALSE);
}


void App::HandleAction(UiAction action, float seekFraction) {
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
    case UiAction::StationheadAudioToggleA:
      ToggleStationheadAudioA();
      break;
    case UiAction::StationheadAudioToggleB:
      ToggleStationheadAudioB();
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
    case UiAction::RadarToggle:
      radar_->TogglePlayback();
      Invalidate(renderer_->RadarRect());
      break;
    case UiAction::RadarPrevious:
      radar_->Previous();
      Invalidate(renderer_->RadarRect());
      break;
    case UiAction::RadarNext:
      radar_->Next();
      Invalidate(renderer_->RadarRect());
      break;
    case UiAction::RadarSeek:
      radar_->SeekFraction(seekFraction);
      Invalidate(renderer_->RadarRect());
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
}  // namespace hp

// Feature groups split out of this file; compiled as part of this translation
// unit so they share its includes and file-local helpers (unity-build pattern,
// like renderer_core.cpp). Not listed in CMake on purpose.
#include "app_air_history.cpp"
#include "app_update.cpp"
#include "app_messages.cpp"
#include "app_commands.cpp"
