#include "app.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {
constexpr int64_t kAirHistoryWindowMs = 24LL * 60 * 60 * 1000;
constexpr int64_t kAirHistoryBucketMs = 5LL * 60 * 1000;
constexpr int64_t kAirHistoryFutureToleranceMs = kAirHistoryBucketMs;
constexpr size_t kAirHistoryMaxSamples =
    static_cast<size_t>(kAirHistoryWindowMs / kAirHistoryBucketMs) + 1;

bool ValidAirValues(const AirHistorySample& sample) noexcept {
  return sample.co2 >= 250 && sample.co2 <= 10000 &&
      sample.temperature >= -40 && sample.temperature <= 85 &&
      sample.humidity >= 0 && sample.humidity <= 100;
}
}  // namespace

void App::LoadAirHistory() {
  const fs::path path = dataDir_ / L"air-history.json";
  try {
    std::ifstream input(path, std::ios::binary);
    if (!input) return;
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    if (text.empty()) return;
    const auto array = winrt::Windows::Data::Json::JsonArray::Parse(Utf8ToWide(text));
    std::vector<AirHistorySample> history;
    const int64_t now = UnixMillis();
    const int64_t cutoff = now - kAirHistoryWindowMs;
    const int64_t futureLimit = now + kAirHistoryFutureToleranceMs;
    for (auto value : array) {
      if (value.ValueType() != winrt::Windows::Data::Json::JsonValueType::Object) continue;
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
      }
    }
    std::sort(history.begin(), history.end(),
              [](const AirHistorySample& left, const AirHistorySample& right) {
                return left.timestamp < right.timestamp;
              });
    history.erase(
        std::unique(history.begin(), history.end(),
                    [](const AirHistorySample& left, const AirHistorySample& right) {
                      return left.timestamp == right.timestamp;
                    }),
        history.end());
    if (history.size() > kAirHistoryMaxSamples) {
      history.erase(history.begin(), history.end() - kAirHistoryMaxSamples);
    }
    renderState_.airHistory = std::move(history);
  } catch (const std::exception& error) {
    if (logger_) logger_->Warn(L"Air history load failed: " + Utf8ToWide(error.what()));
  } catch (...) {
    if (logger_) logger_->Warn(L"Air history load failed with an unknown error");
  }
}

void App::SaveAirHistory() const {
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
    if (!AtomicWriteText(dataDir_ / L"air-history.json", output.str()) && logger_) {
      logger_->Warn(L"Air history atomic write failed");
    }
  } catch (const std::exception& error) {
    if (logger_) logger_->Warn(L"Air history save failed: " + Utf8ToWide(error.what()));
  } catch (...) {
    if (logger_) logger_->Warn(L"Air history save failed with an unknown error");
  }
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
  const auto position = std::lower_bound(
      history.begin(), history.end(), sample.timestamp,
      [](const AirHistorySample& item, int64_t timestamp) {
        return item.timestamp < timestamp;
      });
  if (position != history.end() && position->timestamp == sample.timestamp) return;

  history.insert(position, sample);
  const int64_t cutoff = now - kAirHistoryWindowMs;
  history.erase(
      history.begin(),
      std::lower_bound(
          history.begin(), history.end(), cutoff,
          [](const AirHistorySample& item, int64_t timestamp) {
            return item.timestamp < timestamp;
          }));
  if (history.size() > kAirHistoryMaxSamples) {
    history.erase(history.begin(), history.end() - kAirHistoryMaxSamples);
  }
  SaveAirHistory();
  MarkRenderStateDirty();
}

}  // namespace hp
