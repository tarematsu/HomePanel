





#include "sensors.h"
#include <limits>
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {
constexpr uint64_t kMaxTelemetrySequence = 9'007'199'254'740'990ULL;
}

bool SensorHub::AppendOutbox(const Sample& sample) {
  if (!SampleValuesValid(sample)) return false;
  std::lock_guard lock(mutex_);
  const std::string line = SampleJson(sample) + "\n";
  HANDLE file = CreateFileW(outboxPath_.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok = WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) &&
                  written == line.size();
  CloseHandle(file);
  if (!ok) return false;
  outbox_.push_back(sample);
  state_.outboxCount = outbox_.size();
  return true;
}

void SensorHub::LoadOutbox() {
  std::ifstream ack(outboxAckPath_);
  ack >> acknowledgedSequence_;
  if (acknowledgedSequence_ < std::numeric_limits<uint64_t>::max()) {
    nextSequence_ = std::max(nextSequence_, acknowledgedSequence_ + 1);
  }
  std::ifstream input(outboxPath_);
  std::string line;
  bool repairNeeded = false;
  while (std::getline(input, line)) {
    try {
      const auto object = winrt::Windows::Data::Json::JsonObject::Parse(Utf8ToWide(line));
      Sample sample;
      sample.sequence = static_cast<uint64_t>(object.GetNamedNumber(L"sequence"));
      sample.observedAt = static_cast<int64_t>(object.GetNamedNumber(L"observedAt"));
      sample.co2 = static_cast<int>(object.GetNamedNumber(L"co2"));
      sample.temperature = object.GetNamedNumber(L"temperature");
      sample.humidity = object.GetNamedNumber(L"humidity");
      sample.temperatureCorrected = object.GetNamedNumber(L"temperatureCorrected");
      sample.humidityCorrected = object.GetNamedNumber(L"humidityCorrected");
      if (!SampleValuesValid(sample)) {
        repairNeeded = true;
        continue;
      }
      nextSequence_ = std::max(nextSequence_, sample.sequence + 1);
      lastPersistedBucket_ = std::max(lastPersistedBucket_, sample.observedAt / kTelemetryBucketMs);
      if (sample.sequence > acknowledgedSequence_) outbox_.push_back(sample);
    } catch (...) {
      repairNeeded = true;
    }
  }
  state_.outboxCount = outbox_.size();
  if (repairNeeded) {
    if (RewriteOutboxLocked(outbox_)) log_.Warn(L"Removed invalid CO2 records from telemetry outbox");
    else log_.Warn(L"Failed to repair invalid CO2 telemetry outbox");
  }
}

bool SensorHub::RewriteOutboxLocked(const std::deque<Sample>& samples) {
  std::ostringstream text;
  for (const auto& sample : samples) text << SampleJson(sample) << '\n';
  return AtomicWriteText(outboxPath_, text.str());
}

bool SensorHub::WriteAcknowledgedSequenceLocked(uint64_t sequence) {
  return AtomicWriteText(outboxAckPath_, std::to_string(sequence));
}

void SensorHub::CompactOutboxLocked() {
  std::error_code error;
  const uintmax_t bytes = fs::exists(outboxPath_, error) ? fs::file_size(outboxPath_, error) : 0;
  if (acknowledgedSinceCompaction_ < kCompactAfterAck && bytes < kCompactBytes) return;
  if (RewriteOutboxLocked(outbox_)) acknowledgedSinceCompaction_ = 0;
}

std::string SensorHub::BuildTelemetryPayload(const std::wstring& deviceId, const std::string& appVersion,
                                             bool stationheadOk, size_t maxSamples) {
  std::lock_guard lock(mutex_);
  std::ostringstream out;
  out << "{\"deviceId\":\"" << EscapeJson(WideToUtf8(deviceId)) << "\",\"appVersion\":\""
      << EscapeJson(appVersion) << "\",\"stationheadOk\":" << (stationheadOk ? "true" : "false")
      << ",\"outboxCount\":" << outbox_.size() << ",\"samples\":[";
  const size_t count = std::min(maxSamples, outbox_.size());
  for (size_t i = 0; i < count; ++i) {
    if (i) out << ',';
    out << SampleJson(outbox_[i]);
  }
  out << "]}";
  return out.str();
}

void SensorHub::ApplyTelemetryReceipt(const std::vector<uint64_t>& acknowledgedSequences,
                                      uint64_t nextSequence) {
  std::lock_guard lock(mutex_);
  std::vector<uint64_t> acknowledged = acknowledgedSequences;
  std::sort(acknowledged.begin(), acknowledged.end());
  acknowledged.erase(std::unique(acknowledged.begin(), acknowledged.end()), acknowledged.end());

  uint64_t persistedAck = acknowledgedSequence_;
  if (!acknowledged.empty()) persistedAck = std::max(persistedAck, acknowledged.back());
  if (nextSequence > 0) persistedAck = std::max(persistedAck, nextSequence - 1);

  std::deque<Sample> updated;
  size_t removed = 0;
  for (const auto& sample : outbox_) {
    if (std::binary_search(acknowledged.begin(), acknowledged.end(), sample.sequence)) {
      ++removed;
      continue;
    }
    updated.push_back(sample);
  }

  uint64_t candidate = persistedAck == std::numeric_limits<uint64_t>::max()
      ? persistedAck
      : persistedAck + 1;
  size_t rebased = 0;
  for (auto& sample : updated) {
    if (candidate > kMaxTelemetrySequence) {
      log_.Warn(L"Telemetry sequence space is exhausted; preserving the outbox for manual recovery");
      return;
    }
    if (sample.sequence < candidate) {
      sample.sequence = candidate;
      ++rebased;
    }
    if (sample.sequence >= kMaxTelemetrySequence) {
      candidate = kMaxTelemetrySequence + 1;
    } else {
      candidate = sample.sequence + 1;
    }
  }

  const bool outboxChanged = removed > 0 || rebased > 0;
  if (outboxChanged && !RewriteOutboxLocked(updated)) {
    log_.Warn(L"Failed to persist telemetry acknowledgement; retaining the existing outbox");
    return;
  }
  if (persistedAck > acknowledgedSequence_ && !WriteAcknowledgedSequenceLocked(persistedAck)) {
    log_.Warn(L"Failed to persist the telemetry acknowledgement high-water mark");
  }

  if (outboxChanged) {
    outbox_ = std::move(updated);
    acknowledgedSinceCompaction_ = 0;
  }
  acknowledgedSequence_ = std::max(acknowledgedSequence_, persistedAck);
  nextSequence_ = std::max(nextSequence_, candidate);
  state_.outboxCount = outbox_.size();
  if (rebased > 0) {
    log_.Warn(L"Rebased " + std::to_wstring(rebased) +
              L" telemetry samples above the server sequence high-water mark");
  }
}

}  // namespace hp
