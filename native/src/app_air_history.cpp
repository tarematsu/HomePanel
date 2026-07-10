


#include "app.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {

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
  MarkRenderStateDirty();
}

}
