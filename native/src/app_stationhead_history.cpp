#include "app.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {

void App::LoadStationheadPlayHistory() {
  const fs::path path = dataDir_ / L"stationhead-play-history.json";
  try {
    std::ifstream input(path, std::ios::binary);
    if (!input) return;
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    if (text.empty()) return;
    const auto array = winrt::Windows::Data::Json::JsonArray::Parse(Utf8ToWide(text));
    std::vector<StationheadPlayHistorySample> history;
    constexpr int64_t historyWindowMs = 7 * 24 * 60 * 60 * 1000;
    const int64_t cutoff = UnixMillis() - historyWindowMs;
    for (auto value : array) {
      if (value.ValueType() != winrt::Windows::Data::Json::JsonValueType::Object) continue;
      const auto item = value.GetObject();
      StationheadPlayHistorySample sample{
          static_cast<int64_t>(item.GetNamedNumber(L"t", 0)),
          static_cast<int>(item.GetNamedNumber(L"v", 0)),
      };
      if (sample.timestamp >= cutoff) history.push_back(sample);
    }
    std::sort(history.begin(), history.end(),
              [](const StationheadPlayHistorySample& left, const StationheadPlayHistorySample& right) {
                return left.timestamp < right.timestamp;
              });
    stationheadPlayHistory_ = std::move(history);
  } catch (const std::exception& error) {
    if (logger_) logger_->Warn(L"Stationhead play history load failed: " + Utf8ToWide(error.what()));
  } catch (...) {
    if (logger_) logger_->Warn(L"Stationhead play history load failed with an unknown error");
  }
}

void App::SaveStationheadPlayHistory() const {
  try {
    std::ostringstream output;
    output << "[";
    bool first = true;
    for (const auto& sample : stationheadPlayHistory_) {
      if (!first) output << ",";
      first = false;
      output << "{\"t\":" << sample.timestamp << ",\"v\":" << sample.value << "}";
    }
    output << "]";
    if (!AtomicWriteText(dataDir_ / L"stationhead-play-history.json", output.str()) && logger_) {
      logger_->Warn(L"Stationhead play history atomic write failed");
    }
  } catch (const std::exception& error) {
    if (logger_) logger_->Warn(L"Stationhead play history save failed: " + Utf8ToWide(error.what()));
  } catch (...) {
    if (logger_) logger_->Warn(L"Stationhead play history save failed with an unknown error");
  }
}

void App::UpdateStationheadPlayHistory(const StationheadStatus& status) {
  constexpr int64_t historyWindowMs = 7 * 24 * 60 * 60 * 1000;
  constexpr int64_t sampleBucketMs = 5 * 60 * 1000;
  constexpr size_t maxSamples = static_cast<size_t>(historyWindowMs / sampleBucketMs) + 1;
  if (status.dailyPlayStatsUpdatedAt <= 0 || status.dailyPlayCounts.empty()) return;

  const int64_t bucket = status.dailyPlayStatsUpdatedAt / sampleBucketMs * sampleBucketMs;
  auto& history = stationheadPlayHistory_;
  if (!history.empty() && history.back().timestamp == bucket) return;
  history.push_back({bucket, status.dailyPlayCounts.back().value});

  const int64_t cutoff = UnixMillis() - historyWindowMs;
  history.erase(std::remove_if(history.begin(), history.end(),
                               [cutoff](const StationheadPlayHistorySample& sample) {
                                 return sample.timestamp < cutoff;
                               }),
                history.end());
  if (history.size() > maxSamples) history.erase(history.begin(), history.end() - maxSamples);
  SaveStationheadPlayHistory();
}

}
