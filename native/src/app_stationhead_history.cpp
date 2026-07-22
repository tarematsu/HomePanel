#include "app.h"
#include "stationhead_play_summary.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {

constexpr int64_t kHistoryWindowMs = 7LL * 24 * 60 * 60 * 1000;
constexpr int64_t kSampleBucketMs = 5LL * 60 * 1000;
constexpr int64_t kPersistIntervalMs = 30LL * 60 * 1000;
constexpr size_t kMaxHistorySamples =
    static_cast<size_t>(kHistoryWindowMs / kSampleBucketMs) + 1;

void CompactStationheadPlayHistory(
    std::vector<StationheadPlayHistorySample>& history) {
  if (history.size() < 2) return;

  std::vector<StationheadPlayHistorySample> unique;
  unique.reserve(history.size());
  for (const auto& sample : history) {
    if (!unique.empty() && unique.back().timestamp == sample.timestamp) {
      unique.back() = sample;
    } else {
      unique.push_back(sample);
    }
  }

  std::vector<StationheadPlayHistorySample> compact;
  compact.reserve(unique.size());
  for (const auto& sample : unique) {
    if (compact.empty() || compact.back().value != sample.value) {
      compact.push_back(sample);
      continue;
    }
    if (compact.size() >= 2 &&
        compact[compact.size() - 2].value == sample.value) {
      compact.back() = sample;
    } else {
      compact.push_back(sample);
    }
  }
  history = std::move(compact);
}

}  // namespace

void App::LoadStationheadPlayHistory() {
  const fs::path path = dataDir_ / L"stationhead-play-history.json";
  try {
    std::ifstream input(path, std::ios::binary);
    if (!input) return;
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    if (text.empty()) return;
    const auto array = winrt::Windows::Data::Json::JsonArray::Parse(Utf8ToWide(text));
    std::vector<StationheadPlayHistorySample> history;
    const int64_t now = UnixMillis();
    const int64_t cutoff = now - kHistoryWindowMs;
    for (auto value : array) {
      if (value.ValueType() != winrt::Windows::Data::Json::JsonValueType::Object) continue;
      const auto item = value.GetObject();
      StationheadPlayHistorySample sample{
          static_cast<int64_t>(item.GetNamedNumber(L"t", 0)),
          static_cast<int>(item.GetNamedNumber(L"v", 0)),
      };
      if (sample.timestamp >= cutoff) history.push_back(sample);
    }
    const auto earlier = [](const StationheadPlayHistorySample& left,
                            const StationheadPlayHistorySample& right) {
      return left.timestamp < right.timestamp;
    };
    if (!std::is_sorted(history.begin(), history.end(), earlier)) {
      std::stable_sort(history.begin(), history.end(), earlier);
    }
    CompactStationheadPlayHistory(history);
    if (history.size() > kMaxHistorySamples) {
      history.erase(history.begin(), history.end() - kMaxHistorySamples);
    }
    lastStationheadPlayHistorySavedAt_ = history.empty() ? 0 : now;
    stationheadPlayHistoryDirty_ = false;
    renderState_.stationheadPlayHistory = std::move(history);
    ++renderState_.stationheadPlayHistoryRevision;
  } catch (const std::exception& error) {
    if (logger_) logger_->Warn(L"Stationhead play history load failed: " + Utf8ToWide(error.what()));
  } catch (...) {
    if (logger_) logger_->Warn(L"Stationhead play history load failed with an unknown error");
  }
}

bool App::SaveStationheadPlayHistory() const {
  try {
    std::ostringstream output;
    output << "[";
    bool first = true;
    for (const auto& sample : renderState_.stationheadPlayHistory) {
      if (!first) output << ",";
      first = false;
      output << "{\"t\":" << sample.timestamp << ",\"v\":" << sample.value << "}";
    }
    output << "]";
    if (!AtomicWriteText(dataDir_ / L"stationhead-play-history.json", output.str())) {
      if (logger_) logger_->Warn(L"Stationhead play history atomic write failed");
      return false;
    }
    return true;
  } catch (const std::exception& error) {
    if (logger_) logger_->Warn(L"Stationhead play history save failed: " + Utf8ToWide(error.what()));
  } catch (...) {
    if (logger_) logger_->Warn(L"Stationhead play history save failed with an unknown error");
  }
  return false;
}

void App::UpdateStationheadPlayHistory(const StationheadStatus& status) {
  if (status.dailyPlayStatsUpdatedAt <= 0 || status.dailyPlayCounts.empty()) return;
  if (status.dailyPlayStatsUpdatedAt == lastStationheadPlayStatsUpdatedAt_) return;
  lastStationheadPlayStatsUpdatedAt_ = status.dailyPlayStatsUpdatedAt;

  // The source labels daily counts by UTC date. UTC midnight is 09:00 JST, so
  // selecting the matching UTC day keeps the one-hour history on the same
  // boundary as the Music panel's today/yesterday summaries.
  const int64_t currentDay =
      StationheadUtcDayOrdinal(status.dailyPlayStatsUpdatedAt);
  const auto todayPoint = std::find_if(
      status.dailyPlayCounts.rbegin(), status.dailyPlayCounts.rend(),
      [currentDay](const StationheadDailyPlayPoint& point) {
        return point.dayStartMsUtc > 0 && point.value >= 0 &&
            StationheadUtcDayOrdinal(point.dayStartMsUtc) == currentDay;
      });
  if (todayPoint == status.dailyPlayCounts.rend()) return;

  const int64_t bucket =
      status.dailyPlayStatsUpdatedAt / kSampleBucketMs * kSampleBucketMs;
  const int value = todayPoint->value;
  const int64_t now = UnixMillis();
  const int64_t cutoff = now - kHistoryWindowMs;
  if (bucket < cutoff) return;

  auto& history = renderState_.stationheadPlayHistory;
  const auto position = std::lower_bound(
      history.begin(), history.end(), bucket,
      [](const StationheadPlayHistorySample& sample, int64_t timestamp) {
        return sample.timestamp < timestamp;
      });
  if (position != history.end() && position->timestamp == bucket) {
    if (position->value == value) return;
    position->value = value;
  } else {
    history.insert(position, {bucket, value});
  }

  CompactStationheadPlayHistory(history);
  const auto firstRetained = std::lower_bound(
      history.begin(), history.end(), cutoff,
      [](const StationheadPlayHistorySample& sample, int64_t timestamp) {
        return sample.timestamp < timestamp;
      });
  if (firstRetained != history.begin()) history.erase(history.begin(), firstRetained);
  if (history.size() > kMaxHistorySamples) {
    history.erase(history.begin(), history.end() - kMaxHistorySamples);
  }

  ++renderState_.stationheadPlayHistoryRevision;
  stationheadPlayHistoryDirty_ = true;
  // Do not restore `bucket - lastStationheadPlayHistorySavedAt_ >= kPersistIntervalMs`:
  // source timestamps can lag or be bucket-rounded, so wall-clock time controls persistence.
  if (lastStationheadPlayHistorySavedAt_ <= 0 ||
      now - lastStationheadPlayHistorySavedAt_ >= kPersistIntervalMs) {
    if (SaveStationheadPlayHistory()) {
      stationheadPlayHistoryDirty_ = false;
      lastStationheadPlayHistorySavedAt_ = now;
    }
  }
  MarkRenderStateDirty();
}

}  // namespace hp
