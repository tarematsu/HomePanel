#include "app.h"
#include "cloud_config.h"
#include "version.h"
#include <winrt/Windows.Data.Json.h>
#include <powrprof.h>

namespace hp {
namespace {
constexpr wchar_t kWindowClass[] = L"HomePanelNativeWindow";
constexpr UINT_PTR kCentralTimer = 1;
constexpr UINT WM_HP_UPDATE_RESULT = WM_APP + 20;
constexpr int kRestartExitCode = 42;
constexpr int64_t kSecondaryDashboardSettleMs = 15'000;

std::wstring Quote(const fs::path& path) { return L"\"" + path.wstring() + L"\""; }

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
  SetTimer(window_, kCentralTimer, 1000, nullptr);
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
    logger_->Info(L"Secondary Stationhead prepared; startup waits for primary audio");
  }
  RECT client{};
  if (GetClientRect(window_, &client) && client.right > client.left && client.bottom > client.top) {
    renderer_->Resize(client.right - client.left, client.bottom - client.top);
    LayoutWorkspace();
  }
  stationhead_->Start();
  logger_->Info(L"Stationhead startup was prioritized before dashboard WebView creation");
  // Windows A and B are started together rather than staggering B behind A's
  // confirmed audio + dashboard settle: both are independent WebView2
  // profiles, so there is no real startup-burst contention to avoid, and the
  // user wants both up at the same time.
  if (secondaryStationhead_) {
    secondaryStationhead_->Start();
    secondaryStarted_ = true;
    logger_->Info(L"Secondary Stationhead started alongside primary");
  }

  const std::wstring deviceToken = LoadProtectedToken(dataDir_ / L"device-token.dat", L"HOMEPANEL_DEVICE_TOKEN");
  const std::wstring actionToken = LoadProtectedToken(dataDir_ / L"action-token.dat", L"HOMEPANEL_ACTION_TOKEN");
  cloud_ = std::make_unique<CloudClient>(window_, config_, dataDir_, deviceToken, actionToken, *logger_);
  sensors_ = std::make_unique<SensorHub>(window_, config_, dataDir_, *logger_);
  sensors_->Start();

  LoadAirHistory();
  renderState_.sensors = sensors_->Snapshot();
  UpdateAirHistory(renderState_.sensors);
  renderState_.stationhead = stationhead_->Status();
  renderState_.diagnostics.appVersion = kVersion;
  lastTelemetryAt_ = startupAt_;
  lastDiagnosticAt_ = startupAt_;
  InvalidateAll();
}

void App::StartDeferredServices(int64_t now, const StationheadStatus& stationheadStatus) {
  const bool audioReady = stationheadStatus.audioPlaying || stationheadStatus.lightweight;
  const bool startupDeadlineReached = now - startupAt_ >= 30'000;
  if (audioReady && playbackReadyAt_ == 0) playbackReadyAt_ = now;

  if (!rendererStarted_ && (audioReady || startupDeadlineReached || stationheadStatus.loginRequired)) {
    renderer_->Initialize();
    rendererStarted_ = true;
    logger_->Info(audioReady
        ? L"Dashboard WebView started after Stationhead audio confirmation"
        : L"Dashboard WebView started after startup fallback deadline");
  }

  if (!cloudStarted_ && (audioReady || startupDeadlineReached)) {
    cloud_->Start();
    cloudStarted_ = true;
    logger_->Info(audioReady
        ? L"Cloud synchronization started after Stationhead audio confirmation"
        : L"Cloud synchronization started after startup fallback deadline");
  }

  // Do not compete with the primary player or dashboard WebView during their
  // startup burst. The secondary player is armed only after primary audio has
  // been confirmed and the dashboard has had time to finish its own launch.
  const bool dashboardSettled = rendererStarted_ && playbackReadyAt_ > 0 &&
      now - playbackReadyAt_ >= kSecondaryDashboardSettleMs;
  if (secondaryStationhead_ && audioReady && dashboardSettled && secondaryEligibleAt_ == 0) {
    secondaryEligibleAt_ = now + static_cast<int64_t>(
        std::max(1, config_.stationhead.secondaryStartDelaySeconds)) * 1000;
    logger_->Info(L"Secondary Stationhead queued after primary audio and dashboard startup");
  }
  if (secondaryStationhead_ && !secondaryStarted_ && secondaryEligibleAt_ > 0 &&
      now >= secondaryEligibleAt_) {
    secondaryStationhead_->Start();
    secondaryStarted_ = true;
    logger_->Info(L"Secondary Stationhead startup began after dashboard settle delay");
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

LRESULT CALLBACK App::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
    if (app) app->window_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
  }
  return app ? app->HandleMessage(message, wParam, lParam)
             : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT App::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_TIMER:
      Tick();
      return 0;
    case WM_PAINT:
      Draw();
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      if (renderer_ && wParam != SIZE_MINIMIZED) {
        renderer_->Resize(LOWORD(lParam), HIWORD(lParam));
        LayoutWorkspace();
      }
      return 0;
    case WM_LBUTTONUP: {
      if (!renderer_) return 0;
      POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      float seek = 0;
      HandleAction(renderer_->HitTest(point, &seek), seek);
      return 0;
    }
    case WM_KEYDOWN:
      if (wParam == VK_F12) {
        renderState_.maintenance = !renderState_.maintenance;
        InvalidateAll();
      } else if (wParam == VK_ESCAPE && renderState_.maintenance) {
        renderState_.maintenance = false;
        InvalidateAll();
      }
      return 0;
    case WM_HP_CLOUD_UPDATED: {
      bool dashboardChanged = false;
      if (!renderer_->LoadDashboard(dataDir_ / L"dashboard.json", &dashboardChanged) ||
          !dashboardChanged) {
        return 0;
      }

      renderState_.toast = L"表示データを更新しました";
      const int64_t now = UnixMillis();
      toastUntil_ = now + 4000;

      const int count = renderer_->NewsCount();
      if (count != newsCount_) {
        newsCount_ = count;
        newsIndex_ = 0;
        lastNewsRotateAt_ = count > 1 ? now : 0;
        renderState_.newsIndex = 0;
      }

      const std::wstring monitorHandle = renderer_->MonitorHostHandle();
      if (!monitorHandle.empty() && stationhead_) {
        stationhead_->NotifyMonitorHandle(monitorHandle);
      }
      InvalidateAll();
      return 0;
    }
    case WM_HP_RADAR_UPDATED:
      return 0;
    case WM_HP_SWITCHBOT_UPDATED:
      sensors_->ApplyCloudSwitchBot(dataDir_ / L"switchbot.json");
      renderState_.sensors = sensors_->Snapshot();
      InvalidateAll();
      return 0;
    case WM_HP_SENSOR_UPDATED:
      renderState_.sensors = sensors_->Snapshot();
      UpdateAirHistory(renderState_.sensors);
      InvalidateAll();
      return 0;
    case WM_HP_STATIONHEAD_CHANGED: {
      const uint32_t changes = stationhead_->ConsumeChangeFlags();
      bool layoutChanged = false;
      if ((changes & StationheadChangeReleaseAuth) != 0) {
        stationhead_->ReleaseCompletedAuth();
      }
      if ((changes & StationheadChangeShowPlayer) != 0) {
        stationhead_->ShowAfterAudioStop();
        if (selectedTab_ != WorkspaceTab::Stationhead) {
          selectedTab_ = WorkspaceTab::Stationhead;
          layoutChanged = true;
        }
      } else if ((changes & StationheadChangeReturnMain) != 0 &&
                 selectedTab_ != WorkspaceTab::Main) {
        selectedTab_ = WorkspaceTab::Main;
        layoutChanged = true;
      }
      if (layoutChanged) LayoutWorkspace();
      renderState_.stationhead = stationhead_->Status();
      Invalidate(renderer_->StationheadRect());
      return 0;
    }
    case WM_HP_CONFIG_UPDATED:
      renderState_.toast = L"クラウド設定を保存しました。再起動時に適用します";
      toastUntil_ = UnixMillis() + 5000;
      InvalidateAll();
      return 0;
    case WM_HP_COMMANDS_UPDATED:
      ProcessRemoteCommands();
      return 0;
    case WM_HP_UPDATE_RESULT: {
      std::unique_ptr<wchar_t[]> updateMessage(reinterpret_cast<wchar_t*>(lParam));
      if (updateMessage && updateMessage[0] != L'\0') {
        renderState_.toast = updateMessage.get();
        toastUntil_ = UnixMillis() + 7000;
        InvalidateAll();
      }
      return 0;
    }
    case WM_CLOSE:
      DestroyWindow(window_);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(exitCode_);
      return 0;
  }
  return DefWindowProcW(window_, message, wParam, lParam);
}

void App::Tick() {
  if (!renderer_ || !sensors_ || !stationhead_ || !radar_ || !cloud_) return;
  const int64_t now = UnixMillis();

  stationhead_->Tick(now, renderState_.maintenance);
  if (secondaryStarted_ && secondaryStationhead_) secondaryStationhead_->Tick(now);
  const StationheadStatus stationheadStatus = stationhead_->Status();
  renderState_.stationhead = stationheadStatus;
  // secondaryAudioMuted rides along on the primary status struct so the
  // dashboard can read both windows' mute state from one object; it was
  // declared but never populated, so Window B's mute button never reflected
  // its real state.
  renderState_.stationhead.secondaryAudioMuted =
      secondaryStationhead_ && secondaryStationhead_->AudioMuted();
  StartDeferredServices(now, stationheadStatus);

  if (cloudStarted_ &&
      now - lastTelemetryAt_ >= static_cast<int64_t>(config_.telemetryMinutes) * 60'000) {
    lastTelemetryAt_ = now;
    SendTelemetryAsync();
  }
  const int64_t diagnosticInterval = renderState_.maintenance ? 5'000 : 30 * 60'000;
  if (now - lastDiagnosticAt_ >= diagnosticInterval) {
    lastDiagnosticAt_ = now;
    RefreshDiagnostics();
    if (renderState_.maintenance) InvalidateAll();
  }
  if (toastUntil_ && now >= toastUntil_) {
    toastUntil_ = 0;
    renderState_.toast.clear();
    InvalidateAll();
  }
  if (newsCount_ > 1 && lastNewsRotateAt_ > 0 && now - lastNewsRotateAt_ >= 30'000) {
    lastNewsRotateAt_ = now;
    newsIndex_ = (newsIndex_ + 1) % newsCount_;
    renderState_.newsIndex = newsIndex_;
    InvalidateAll();
  }
}

void App::Draw() {
  PAINTSTRUCT paint{};
  BeginPaint(window_, &paint);
  if (renderer_) {
    // Message handlers own state updates. Avoid sensor locks and Stationhead status
    // reconstruction on every incidental WM_PAINT.
    renderer_->Render(paint.rcPaint, renderState_);
  }
  EndPaint(window_, &paint);
}

void App::LayoutWorkspace() {
  if (!renderer_ || !stationhead_) return;
  RECT client{};
  GetClientRect(window_, &client);
  workspaceBounds_ = client;
  renderer_->SetBounds(workspaceBounds_);
  renderer_->SetVisible(true);

  const int clientWidth = std::max(1L, client.right - client.left);
  const int clientHeight = std::max(1L, client.bottom - client.top);
  const int split = client.left + clientWidth / 2;
  stationhead_->SetBounds(RECT{client.left, client.top, split, client.top + clientHeight});
  if (secondaryStationhead_) {
    secondaryStationhead_->SetBounds(
        RECT{split, client.top, client.left + clientWidth, client.top + clientHeight});
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
  InvalidateAll();
}

void App::PublishRenderState() {
  if (renderer_) renderer_->UpdateState(renderState_);
}

void App::InvalidateAll() {
  PublishRenderState();
  ::InvalidateRect(window_, nullptr, FALSE);
}

void App::Invalidate(const RECT& rect) {
  PublishRenderState();
  ::InvalidateRect(window_, &rect, FALSE);
}

void App::LoadAirHistory() {
  const fs::path path = dataDir_ / L"air-history.json";
  try {
    std::ifstream input(path, std::ios::binary);
    if (!input) return;
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    if (text.empty()) return;
    const auto array = winrt::Windows::Data::Json::JsonArray::Parse(Utf8ToWide(text));
    std::vector<AirHistorySample> history;
    const int64_t cutoff = UnixMillis() - 24 * 60 * 60 * 1000;
    for (auto value : array) {
      if (value.ValueType() != winrt::Windows::Data::Json::JsonValueType::Object) continue;
      const auto item = value.GetObject();
      AirHistorySample sample{
          static_cast<int64_t>(item.GetNamedNumber(L"t", 0)),
          static_cast<int>(item.GetNamedNumber(L"co2", 0)),
          item.GetNamedNumber(L"temperature", 0),
          item.GetNamedNumber(L"humidity", 0),
      };
      if (sample.timestamp >= cutoff && sample.co2 >= 250 && sample.co2 <= 10000 &&
          sample.temperature >= -40 && sample.temperature <= 85 &&
          sample.humidity >= 0 && sample.humidity <= 100) {
        history.push_back(sample);
      }
    }
    std::sort(history.begin(), history.end(), [](const AirHistorySample& left, const AirHistorySample& right) {
      return left.timestamp < right.timestamp;
    });
    renderState_.airHistory = std::move(history);
  } catch (const std::exception& error) {
    if (logger_) logger_->Warn(L"Air history load failed: " + Utf8ToWide(error.what()));
  } catch (...) {
    if (logger_) logger_->Warn(L"Air history load failed with an unknown error");
  }
}

void App::SaveAirHistory() const {
  const fs::path target = dataDir_ / L"air-history.json";
  const fs::path temporary = dataDir_ / L"air-history.json.tmp";
  try {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return;
    output << "[";
    bool first = true;
    for (const auto& sample : renderState_.airHistory) {
      if (!first) output << ",";
      first = false;
      output << "{\"t\":" << sample.timestamp
             << ",\"co2\":" << sample.co2
             << ",\"temperature\":" << sample.temperature
             << ",\"humidity\":" << sample.humidity << "}";
    }
    output << "]";
    output.close();
    if (!output) return;
    std::error_code ignored;
    fs::rename(temporary, target, ignored);
    if (ignored) {
      ignored.clear();
      fs::copy_file(temporary, target, fs::copy_options::overwrite_existing, ignored);
      fs::remove(temporary, ignored);
    }
  } catch (const std::exception& error) {
    if (logger_) logger_->Warn(L"Air history save failed: " + Utf8ToWide(error.what()));
  } catch (...) {
    if (logger_) logger_->Warn(L"Air history save failed with an unknown error");
  }
}

void App::UpdateAirHistory(const SensorSnapshot& sensors) {
  constexpr int64_t historyWindowMs = 24 * 60 * 60 * 1000;
  constexpr int64_t sampleBucketMs = 5 * 60 * 1000;
  constexpr size_t maxSamples = static_cast<size_t>(historyWindowMs / sampleBucketMs) + 1;
  if (!sensors.co2Connected || sensors.observedAt <= 0 || sensors.co2 < 250 || sensors.co2 > 10000 ||
      sensors.temperatureCorrected < -40 || sensors.temperatureCorrected > 85 ||
      sensors.humidityCorrected < 0 || sensors.humidityCorrected > 100) {
    return;
  }

  const int64_t bucket = sensors.observedAt / sampleBucketMs * sampleBucketMs;
  auto& history = renderState_.airHistory;
  if (!history.empty() && history.back().timestamp == bucket) return;
  history.push_back({bucket, sensors.co2, sensors.temperatureCorrected, sensors.humidityCorrected});
  const int64_t cutoff = UnixMillis() - historyWindowMs;
  history.erase(std::remove_if(history.begin(), history.end(), [cutoff](const AirHistorySample& sample) {
    return sample.timestamp < cutoff;
  }), history.end());
  if (history.size() > maxSamples) history.erase(history.begin(), history.end() - maxSamples);
  SaveAirHistory();
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
        InvalidateAll();
      }
      break;
    case UiAction::DataRefresh:
      renderState_.toast = cloud_->RequestRemoteRefresh()
        ? L"Cloudflareへ更新を要求しました" : L"更新要求に失敗しました";
      toastUntil_ = UnixMillis() + 4000;
      InvalidateAll();
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
      InvalidateAll();
      break;
    case UiAction::StationheadReconnect:
      stationhead_->Reconnect();
      break;
    case UiAction::ClearCache:
      ClearDisplayCache();
      break;
    case UiAction::ShowLog:
      ShellExecuteW(window_, L"open", L"notepad.exe", Quote(dataDir_ / L"homepanel.log").c_str(),
                    rootDir_.c_str(), SW_SHOWNORMAL);
      break;
    case UiAction::CloseMaintenance:
      renderState_.maintenance = false;
      InvalidateAll();
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

void App::ProcessRemoteCommands() {
  const fs::path path = dataDir_ / L"commands.json";
  try {
    std::ifstream input(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(input)), {});
    if (text.empty()) return;
    auto root = winrt::Windows::Data::Json::JsonObject::Parse(Utf8ToWide(text));
    if (!root.HasKey(L"commands")) return;
    for (auto value : root.GetNamedArray(L"commands")) {
      auto item = value.GetObject();
      const int64_t id = static_cast<int64_t>(item.GetNamedNumber(L"id", 0));
      const std::wstring command = item.GetNamedString(L"command", L"").c_str();
      if (id <= 0 || command.empty()) continue;
      bool success = true;
      std::wstring result = L"completed";
      if (command == L"restart_app") {
        if (!cloud_->AcknowledgeCommand(id, true, L"restarting")) {
          logger_->Warn(L"Restart command acknowledgement failed; restart postponed");
          continue;
        }
        std::error_code ignored;
        fs::remove(path, ignored);
        exitCode_ = kRestartExitCode;
        DestroyWindow(window_);
        return;
      } else if (command == L"reconnect_stationhead") {
        stationhead_->Reconnect();
      } else if (command == L"clear_display_cache") {
        ClearDisplayCache();
      } else if (command == L"reload_dashboard") {
        cloud_->RefreshNow();
      } else if (command == L"check_update") {
        CheckForUpdateAsync(true);
        result = L"verified update check started";
      } else {
        success = false;
        result = L"unknown command";
      }
      cloud_->AcknowledgeCommand(id, success, result);
    }
    std::error_code ignored;
    fs::remove(path, ignored);
  } catch (const std::exception& error) {
    logger_->Warn(L"Remote command processing failed: " + Utf8ToWide(error.what()));
    std::error_code ignored;
    fs::remove(path, ignored);
  }
}

void App::SendTelemetryAsync() {
  if (telemetryBusy_.exchange(true)) return;
  if (telemetryThread_.joinable()) telemetryThread_.join();
  const double cpuPercent = renderState_.diagnostics.cpuPercent;
  telemetryThread_ = std::thread([this, cpuPercent] {
    try {
      const auto sensor = sensors_->Snapshot();
      const auto station = stationhead_->Status();
      const size_t count = std::min<size_t>(500, sensor.outboxCount);
      std::string body = sensors_->BuildTelemetryPayload(
          config_.deviceId, WideToUtf8(kVersion), station.playing, count);
      MEMORYSTATUSEX memory{sizeof(memory)};
      GlobalMemoryStatusEx(&memory);
      if (!body.empty() && body.back() == '}') body.pop_back();
      std::ostringstream diagnostics;
      diagnostics << ",\"diagnostics\":{" 
        << "\"appWorkingSet\":" << ProcessWorkingSet(GetCurrentProcessId()) << ','
        << "\"webViewWorkingSet\":" << station.processWorkingSet << ','
        << "\"webViewCpuPercent\":" << std::fixed << std::setprecision(2) << station.processCpuPercent << ','
        << "\"availablePhysical\":" << memory.ullAvailPhys << ','
        << "\"cpuPercent\":" << std::fixed << std::setprecision(2) << cpuPercent << ','
        << "\"co2Connected\":" << (sensor.co2Connected ? "true" : "false") << ','
        << "\"lastCo2At\":" << sensor.observedAt << ','
        << "\"stationheadPlaying\":" << (station.playing ? "true" : "false") << ','
        << "\"lastStationheadAt\":" << station.lastPlaybackConfirmedAt << ','
        << "\"cloudFailures\":" << cloud_->ConsecutiveFailures()
        << "}}";
      body += diagnostics.str();
      if (cloud_->SendTelemetry(body) && count) sensors_->AcknowledgeTelemetry(count);
    } catch (const std::exception& error) {
      if (logger_) logger_->Warn(L"Telemetry worker failed: " + Utf8ToWide(error.what()));
    } catch (...) {
      if (logger_) logger_->Warn(L"Telemetry worker failed with an unknown exception");
    }
    telemetryBusy_ = false;
  });
}

void App::RefreshDiagnostics() {
  MEMORYSTATUSEX memory{sizeof(memory)};
  GlobalMemoryStatusEx(&memory);
  renderState_.diagnostics.appWorkingSet = ProcessWorkingSet(GetCurrentProcessId());
  renderState_.diagnostics.webViewWorkingSet = stationhead_ ? stationhead_->Status().processWorkingSet : 0;
  renderState_.diagnostics.availablePhysical = memory.ullAvailPhys;
  renderState_.diagnostics.cpuPercent = ProcessCpuPercent();
  renderState_.diagnostics.cloudLastSuccess = cloud_ ? cloud_->LastSuccessText() : L"";
  renderState_.diagnostics.workerVersion = cloud_ ? cloud_->WorkerVersion() : L"";
  renderState_.diagnostics.appVersion = kVersion;
  renderState_.diagnostics.co2LastTime = FormatTimestamp(sensors_ ? sensors_->Snapshot().observedAt : 0);
  renderState_.diagnostics.stationheadLastTime = FormatTimestamp(
      stationhead_ ? stationhead_->Status().lastPlaybackConfirmedAt : 0);
  const int64_t now = UnixMillis();
  if (logger_ && now - lastPerformanceLogAt_ >= 5 * 60'000) {
    lastPerformanceLogAt_ = now;
    std::wostringstream metric;
    metric << L"Performance appWS=" << renderState_.diagnostics.appWorkingSet / 1048576.0
           << L"MB webviewWS=" << renderState_.diagnostics.webViewWorkingSet / 1048576.0
           << L"MB available=" << renderState_.diagnostics.availablePhysical / 1048576.0
           << L"MB cpu=" << std::fixed << std::setprecision(2)
           << renderState_.diagnostics.cpuPercent << L"%";
    logger_->Info(metric.str());
  }
}

void App::ClearDisplayCache() {
  std::error_code ignored;
  fs::remove(dataDir_ / L"dashboard.json", ignored);
  fs::remove(dataDir_ / L"radar.json", ignored);
  fs::remove_all(dataDir_ / L"radar-cache", ignored);
  radar_->ReloadMetadata();
  cloud_->RefreshNow();
  renderState_.toast = L"表示キャッシュを削除しました。ログイン情報と履歴は保持しています";
  toastUntil_ = UnixMillis() + 5000;
  InvalidateAll();
  logger_->Info(L"Display cache cleared; WebView user data and telemetry outbox preserved");
}

void App::CheckForUpdateAsync(bool install) {
  if (updateBusy_.exchange(true)) {
    if (install) {
      renderState_.toast = L"更新確認はすでに実行中です";
      toastUntil_ = UnixMillis() + 4000;
      InvalidateAll();
    }
    return;
  }
  if (updateThread_.joinable()) updateThread_.join();
  if (install) {
    renderState_.toast = L"署名・ハッシュを確認して更新を準備しています";
    toastUntil_ = UnixMillis() + 15'000;
    InvalidateAll();
  }

  updateThread_ = std::thread([this, install] {
    std::wstring message;
    try {
      const std::string manifestJson = cloud_->FetchUpdateManifest();
      const UpdateManifest manifest = ParseUpdateManifest(manifestJson);
      if (!IsVersionNewer(manifest.version, kVersion)) {
        if (install) {
          message = L"すでに最新バージョンです (v" + std::wstring(kVersion) + L")";
        }
      } else if (!install) {
        message = L"HomePanel " + manifest.version + L" が利用できます";
      } else if (LaunchVerifiedUpdater(manifest.version, manifestJson)) {
        logger_->Info(L"Verified updater launched for version " + manifest.version);
        PostMessageW(window_, WM_CLOSE, 0, 0);
        updateBusy_ = false;
        return;
      } else {
        message = L"検証済み更新プログラムを起動できませんでした";
      }
    } catch (const std::exception& error) {
      logger_->Warn(L"Update check failed: " + Utf8ToWide(error.what()));
      if (install) message = L"更新確認に失敗: " + Utf8ToWide(error.what());
    }

    if (!message.empty()) {
      auto copy = std::make_unique<wchar_t[]>(message.size() + 1);
      wcscpy_s(copy.get(), message.size() + 1, message.c_str());
      if (PostMessageW(window_, WM_HP_UPDATE_RESULT, 0, reinterpret_cast<LPARAM>(copy.get()))) {
        copy.release();
      }
    }
    updateBusy_ = false;
  });
}

bool App::LaunchVerifiedUpdater(const std::wstring& version, const std::string& manifestJson) {
  const fs::path installedUpdater = rootDir_ / L"HomePanelUpdater.exe";
  if (!fs::exists(installedUpdater)) {
    logger_->Warn(L"HomePanelUpdater.exe is not installed; one manual package update is required");
    return false;
  }

  const fs::path pending = dataDir_ / L"pending-update.json";
  const fs::path temporary = dataDir_ / L"pending-update.json.tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(manifestJson.data(), static_cast<std::streamsize>(manifestJson.size()));
    output.flush();
    if (!output) return false;
  }
  if (!MoveFileExW(temporary.c_str(), pending.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temporary.c_str());
    return false;
  }

  const fs::path runnerDirectory = dataDir_ / L"update-runner";
  const fs::path runner = runnerDirectory / L"HomePanelUpdater.exe";
  std::error_code ignored;
  fs::create_directories(runnerDirectory, ignored);
  if (ignored || !CopyFileW(installedUpdater.c_str(), runner.c_str(), FALSE)) {
    logger_->Warn(L"Failed to stage the update runner: " + std::to_wstring(GetLastError()));
    return false;
  }

  std::wstring command = Quote(runner) + L" --pid " + std::to_wstring(GetCurrentProcessId()) +
      L" --root " + Quote(rootDir_) + L" --manifest " + Quote(pending) + L" --version " + version;
  std::vector<wchar_t> buffer(command.begin(), command.end());
  buffer.push_back(L'\0');
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(runner.c_str(), buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, rootDir_.c_str(), &startup, &process)) {
    logger_->Warn(L"CreateProcess for updater failed: " + std::to_wstring(GetLastError()));
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

std::wstring App::FormatTimestamp(int64_t timestamp) {
  if (!timestamp) return L"-";
  __time64_t seconds = timestamp / 1000;
  tm local{};
  _localtime64_s(&local, &seconds);
  wchar_t output[64]{};
  wcsftime(output, _countof(output), L"%Y-%m-%d %H:%M:%S", &local);
  return output;
}

size_t App::ProcessWorkingSet(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!process) return 0;
  PROCESS_MEMORY_COUNTERS_EX counters{sizeof(counters)};
  size_t value = 0;
  if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
    value = counters.WorkingSetSize;
  }
  CloseHandle(process);
  return value;
}

double App::ProcessCpuPercent() {
  static ULARGE_INTEGER previousKernel{}, previousUser{};
  static int64_t previousWall = 0;
  FILETIME creation{}, exit{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return 0;
  ULARGE_INTEGER currentKernel{kernel.dwLowDateTime, kernel.dwHighDateTime};
  ULARGE_INTEGER currentUser{user.dwLowDateTime, user.dwHighDateTime};
  const int64_t now = UnixMillis();
  double result = 0;
  if (previousWall) {
    const uint64_t cpu100ns =
        (currentKernel.QuadPart - previousKernel.QuadPart) + (currentUser.QuadPart - previousUser.QuadPart);
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    result = (cpu100ns / 10000.0) / std::max<int64_t>(1, now - previousWall) * 100.0 /
             std::max<DWORD>(1, system.dwNumberOfProcessors);
  }
  previousKernel = currentKernel;
  previousUser = currentUser;
  previousWall = now;
  return result;
}

void App::LogUnhandled(DWORD code, void* address) {
  if (logger_) {
    std::wostringstream text;
    text << L"Unhandled exception 0x" << std::hex << code << L" at " << address;
    logger_->Error(text.str());
  }
}
}  // namespace hp
