// Part of app.cpp's translation unit (see the #include at the end of that
// file). Cloud-driven housekeeping: remote command processing, async telemetry
// upload, and display-cache clearing. Uses the kRestartExitCode constant from
// app.cpp.
#include "app.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {

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
  telemetryThread_ = std::thread([this] {
    try {
      const auto sensor = sensors_->Snapshot();
      const auto station = stationhead_->Status();
      const size_t count = std::min<size_t>(500, sensor.outboxCount);
      std::string body = sensors_->BuildTelemetryPayload(
          config_.deviceId, WideToUtf8(kVersion), station.playing, count);
      if (cloud_->SendTelemetry(body) && count) sensors_->AcknowledgeTelemetry(count);
    } catch (const std::exception& error) {
      if (logger_) logger_->Warn(L"Telemetry worker failed: " + Utf8ToWide(error.what()));
    } catch (...) {
      if (logger_) logger_->Warn(L"Telemetry worker failed with an unknown exception");
    }
    telemetryBusy_ = false;
  });
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
  MarkRenderStateDirty();
  InvalidateAll();
  logger_->Info(L"Display cache cleared; WebView user data and telemetry outbox preserved");
}

}  // namespace hp
