



#include "app.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {
using PendingCommandAcks = std::map<int64_t, bool>;

PendingCommandAcks LoadPendingCommandAcks(const fs::path& path) {
  PendingCommandAcks pending;
  std::ifstream input(path, std::ios::binary);
  int64_t id = 0;
  int success = 0;
  while (input >> id >> success) {
    if (id > 0) pending[id] = success != 0;
  }
  return pending;
}

bool SavePendingCommandAcks(const fs::path& path, const PendingCommandAcks& pending) {
  if (pending.empty()) {
    std::error_code ignored;
    fs::remove(path, ignored);
    return true;
  }
  std::ostringstream output;
  for (const auto& [id, success] : pending) {
    output << id << ' ' << (success ? 1 : 0) << '\n';
  }
  return AtomicWriteText(path, output.str());
}
}

void App::ProcessRemoteCommands() {
  const fs::path path = dataDir_ / L"commands.json";
  const fs::path pendingAckPath = dataDir_ / L"command-acks.pending";
  PendingCommandAcks pendingAcks = LoadPendingCommandAcks(pendingAckPath);
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

      if (const auto pending = pendingAcks.find(id); pending != pendingAcks.end()) {
        if (cloud_->AcknowledgeCommand(id, pending->second, L"completed earlier; acknowledgement retry")) {
          pendingAcks.erase(pending);
          if (!SavePendingCommandAcks(pendingAckPath, pendingAcks)) {
            logger_->Warn(L"Failed to persist command acknowledgement retry state");
          }
        }
        continue;
      }

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
        if (secondaryStationhead_) secondaryStationhead_->Reconnect();
        result = secondaryStationhead_
            ? L"primary and secondary Stationhead reconnect started"
            : L"primary Stationhead reconnect started";
      } else if (command == L"clear_display_cache") {
        ClearDisplayCache();
      } else if (command == L"reload_dashboard") {
        cloud_->RefreshNow();
      } else if (command == L"check_update") {



        if (updateBusy_.load(std::memory_order_acquire)) {
          logger_->Info(L"Update install command deferred because an update check is already running");
          continue;
        }
        CheckForUpdateAsync(true);
        result = L"verified update check started";
      } else {
        success = false;
        result = L"unknown command";
      }





      pendingAcks[id] = success;
      if (!SavePendingCommandAcks(pendingAckPath, pendingAcks)) {
        logger_->Warn(L"Failed to persist completed remote command " + std::to_wstring(id));
      }
      if (cloud_->AcknowledgeCommand(id, success, result)) {
        pendingAcks.erase(id);
        if (!SavePendingCommandAcks(pendingAckPath, pendingAcks)) {
          logger_->Warn(L"Failed to clear completed remote command acknowledgement " + std::to_wstring(id));
        }
      } else {
        logger_->Warn(L"Remote command acknowledgement deferred without re-execution: " + std::to_wstring(id));
      }
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
      const TelemetryReceipt receipt = cloud_->SendTelemetry(body);
      if (receipt.success) {
        sensors_->ApplyTelemetryReceipt(receipt.acknowledgedSequences, receipt.nextSequence);
      }
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
  cloud_->RefreshNow();
  renderState_.toast = L"表示キャッシュを削除しました。ログイン情報と履歴は保持しています";
  ShowToast(std::move(renderState_.toast), 5000);
  logger_->Info(L"Display cache cleared; WebView user data and telemetry outbox preserved");
}

}
