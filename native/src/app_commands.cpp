#include "app.h"
#include "version.h"
#include <charconv>
#include <limits>
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {
using PendingCommandAcks = std::map<int64_t, bool>;
constexpr std::uintmax_t kMaxCommandFileBytes = 4 * 1024 * 1024;

std::optional<std::string> ReadCommandFile(const fs::path& path) {
  std::error_code error;
  const std::uintmax_t size = fs::file_size(path, error);
  if (error || size > kMaxCommandFileBytes ||
      size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
    return std::nullopt;
  }
  std::string text(static_cast<size_t>(size), '\0');
  if (text.empty()) return text;
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  input.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (input.gcount() != static_cast<std::streamsize>(text.size()) || input.peek() != EOF) {
    return std::nullopt;
  }
  return text;
}

bool AppendInteger(std::string& output, int64_t value) {
  char buffer[32]{};
  const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value);
  if (result.ec != std::errc{}) return false;
  output.append(buffer, result.ptr);
  return true;
}

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
  std::string output;
  output.reserve(pending.size() * 24);
  for (const auto& [id, success] : pending) {
    if (!AppendInteger(output, id)) return false;
    output.push_back(' ');
    output.push_back(success ? '1' : '0');
    output.push_back('\n');
  }
  return AtomicWriteText(path, output);
}
}  // namespace

void App::ProcessRemoteCommands() {
  const fs::path path = dataDir_ / L"commands.json";
  const fs::path pendingAckPath = dataDir_ / L"command-acks.pending";
  PendingCommandAcks pendingAcks = LoadPendingCommandAcks(pendingAckPath);
  try {
    const std::optional<std::string> text = ReadCommandFile(path);
    if (!text || text->empty()) return;
    auto root = winrt::Windows::Data::Json::JsonObject::Parse(Utf8ToWide(*text));
    if (!root.HasKey(L"commands")) return;
    const auto commands = root.GetNamedArray(L"commands");
    for (const auto value : commands) {
      auto item = value.GetObject();
      const int64_t id = static_cast<int64_t>(item.GetNamedNumber(L"id", 0));
      const std::wstring command = item.GetNamedString(L"command", L"").c_str();
      if (id <= 0 || command.empty()) continue;

      if (const auto pending = pendingAcks.find(id); pending != pendingAcks.end()) {
        if (cloud_->AcknowledgeCommand(
                id, pending->second, L"completed earlier; acknowledgement retry")) {
          pendingAcks.erase(pending);
          if (!SavePendingCommandAcks(pendingAckPath, pendingAcks)) {
            logger_->Warn(L"Failed to persist command acknowledgement retry state");
          }
        }
        continue;
      }

      bool success = command == L"check_update";
      std::wstring result = success ? L"verified update check started" : L"unsupported command";
      if (success) {
        if (updateBusy_.load(std::memory_order_acquire)) {
          logger_->Info(L"Update install command deferred because an update check is already running");
          continue;
        }
        CheckForUpdateAsync(true);
      }

      pendingAcks[id] = success;
      if (!SavePendingCommandAcks(pendingAckPath, pendingAcks)) {
        logger_->Warn(L"Failed to persist completed remote command " + std::to_wstring(id));
      }
      if (cloud_->AcknowledgeCommand(id, success, result)) {
        pendingAcks.erase(id);
        if (!SavePendingCommandAcks(pendingAckPath, pendingAcks)) {
          logger_->Warn(
              L"Failed to clear completed remote command acknowledgement " +
              std::to_wstring(id));
        }
      } else {
        logger_->Warn(
            L"Remote command acknowledgement deferred without re-execution: " +
            std::to_wstring(id));
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
  try {
    static const std::string versionUtf8 = WideToUtf8(kVersion);
    const auto sensor = sensors_->Snapshot();
    const bool stationPlaying = stationhead_->AudioPlaying();
    const size_t count = std::min<size_t>(60, sensor.outboxCount);
    const std::string body = sensors_->BuildTelemetryPayload(
        config_.deviceId, versionUtf8, stationPlaying, count);
    cloud_->QueueTelemetry(body, [this](TelemetryReceipt receipt) {
      if (receipt.success && sensors_) {
        sensors_->ApplyTelemetryReceipt(receipt.acknowledgedSequences, receipt.nextSequence);
      }
    });
  } catch (const std::exception& error) {
    if (logger_) logger_->Warn(L"Telemetry queue failed: " + Utf8ToWide(error.what()));
  } catch (...) {
    if (logger_) logger_->Warn(L"Telemetry queue failed with an unknown exception");
  }
}

void App::ClearDisplayCache() {
  std::error_code ignored;
  fs::remove(dataDir_ / L"dashboard.json", ignored);
  fs::remove(dataDir_ / L"radar.json", ignored);
  fs::remove_all(dataDir_ / L"radar-cache", ignored);
  cloud_->RefreshNow();
  ShowToast(L"表示キャッシュを削除しました。ログイン情報と履歴は保持しています", 5000);
  logger_->Info(L"Display cache cleared; WebView user data and telemetry outbox preserved");
}

}  // namespace hp
