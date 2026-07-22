#include "app.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {
constexpr int64_t kAirHistoryWindowMs = 24LL * 60 * 60 * 1000;
constexpr int64_t kAirHistoryBucketMs = 5LL * 60 * 1000;
constexpr int64_t kAirHistoryPersistIntervalMs = 30LL * 60 * 1000;
constexpr int64_t kAirHistoryFutureToleranceMs = kAirHistoryBucketMs;
constexpr size_t kAirHistoryMaxSamples =
    static_cast<size_t>(kAirHistoryWindowMs / kAirHistoryBucketMs) + 1;

bool ValidAirValues(const AirHistorySample& sample) noexcept {
  return sample.co2 >= 250 && sample.co2 <= 10000 &&
      sample.temperature >= -40 && sample.temperature <= 85 &&
      sample.humidity >= 0 && sample.humidity <= 100;
}

bool AirHistoryEarlier(const AirHistorySample& left,
                       const AirHistorySample& right) noexcept {
  return left.timestamp < right.timestamp;
}

bool AirHistoryBeforeTimestamp(const AirHistorySample& sample,
                               int64_t timestamp) noexcept {
  return sample.timestamp < timestamp;
}
}  // namespace

App::HistoryFlushGuard::~HistoryFlushGuard() {
  if (!owner) return;
  const int64_t now = UnixMillis();
  if (owner->airHistoryDirty_ && owner->SaveAirHistory()) {
    owner->airHistoryDirty_ = false;
    owner->lastAirHistorySavedAt_ = now;
  }
  if (owner->stationheadPlayHistoryDirty_ &&
      owner->SaveStationheadPlayHistory()) {
    owner->stationheadPlayHistoryDirty_ = false;
    owner->lastStationheadPlayHistorySavedAt_ = now;
  }
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
    bool normalized = false;
    const int64_t now = UnixMillis();
    const int64_t cutoff = now - kAirHistoryWindowMs;
    const int64_t futureLimit = now + kAirHistoryFutureToleranceMs;
    for (auto value : array) {
      if (value.ValueType() != winrt::Windows::Data::Json::JsonValueType::Object) {
        normalized = true;
        continue;
      }
      const auto item = value.GetObject();
      AirHistorySample sample{
          static_cast<int64_t>(item.GetNamedNumber(L"t", 0)),
          static_cast<int>(item.GetNamedNumber(L"co2", 0)),
          item.GetNamedNumber(L"temperature", 0),
          item.GetNamedNumber(L"humidity", 0),
      };
      if (sample.timestamp >= cutoff && sample.timestamp <= futureLimit &&
          ValidAirValues(sample)) {
        history.push_back(sample);
      } else {
        normalized = true;
      }
    }
    if (!std::is_sorted(history.begin(), history.end(), AirHistoryEarlier)) {
      normalized = true;
      std::stable_sort(history.begin(), history.end(), AirHistoryEarlier);
    }
    const auto uniqueEnd = std::unique(
        history.begin(), history.end(),
        [](const AirHistorySample& left, const AirHistorySample& right) {
          return left.timestamp == right.timestamp;
        });
    if (uniqueEnd != history.end()) {
      normalized = true;
      history.erase(uniqueEnd, history.end());
    }
    if (history.size() > kAirHistoryMaxSamples) {
      normalized = true;
      history.erase(history.begin(), history.end() - kAirHistoryMaxSamples);
    }
    renderState_.airHistory = std::move(history);
    ++renderState_.airHistoryRevision;
    airHistoryDirty_ = normalized;
    if (airHistoryDirty_ && SaveAirHistory()) airHistoryDirty_ = false;
    lastAirHistorySavedAt_ = airHistoryDirty_ ? 0 : now;
  } catch (const std::exception& error) {
    if (logger_) logger_->Warn(L"Air history load failed: " + Utf8ToWide(error.what()));
  } catch (...) {
    if (logger_) logger_->Warn(L"Air history load failed with an unknown error");
  }
}

bool App::SaveAirHistory() const {
  try {
    std::ostringstream output;
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
    if (!AtomicWriteText(dataDir_ / L"air-history.json", output.str())) {
      if (logger_) logger_->Warn(L"Air history atomic write failed");
      return false;
    }
    return true;
  } catch (const std::exception& error) {
    if (logger_) logger_->Warn(L"Air history save failed: " + Utf8ToWide(error.what()));
  } catch (...) {
    if (logger_) logger_->Warn(L"Air history save failed with an unknown error");
  }
  return false;
}

void App::UpdateAirHistory(const SensorSnapshot& sensors) {
  const int64_t now = UnixMillis();
  const AirHistorySample sample{
      sensors.observedAt / kAirHistoryBucketMs * kAirHistoryBucketMs,
      sensors.co2,
      sensors.temperatureCorrected,
      sensors.humidityCorrected,
  };
  if (!sensors.co2Connected || sensors.observedAt <= 0 ||
      sensors.observedAt > now + kAirHistoryFutureToleranceMs ||
      sample.timestamp < now - kAirHistoryWindowMs || !ValidAirValues(sample)) {
    return;
  }

  auto& history = renderState_.airHistory;
  if (!history.empty() && history.back().timestamp == sample.timestamp) return;
  if (history.empty() || history.back().timestamp < sample.timestamp) {
    history.push_back(sample);
  } else {
    const auto position = std::lower_bound(
        history.begin(), history.end(), sample.timestamp, AirHistoryBeforeTimestamp);
    if (position != history.end() && position->timestamp == sample.timestamp) return;
    history.insert(position, sample);
  }

  const int64_t cutoff = now - kAirHistoryWindowMs;
  const auto firstRetained = std::lower_bound(
      history.begin(), history.end(), cutoff, AirHistoryBeforeTimestamp);
  if (firstRetained != history.begin()) history.erase(history.begin(), firstRetained);
  if (history.size() > kAirHistoryMaxSamples) {
    history.erase(history.begin(), history.end() - kAirHistoryMaxSamples);
  }
  ++renderState_.airHistoryRevision;
  airHistoryDirty_ = true;
  if (lastAirHistorySavedAt_ <= 0 ||
      now - lastAirHistorySavedAt_ >= kAirHistoryPersistIntervalMs) {
    if (SaveAirHistory()) {
      airHistoryDirty_ = false;
      lastAirHistorySavedAt_ = now;
    }
  }
  MarkRenderStateDirty();
}

}  // namespace hp
