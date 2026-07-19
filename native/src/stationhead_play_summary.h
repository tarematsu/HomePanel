#pragma once
#include "sh.h"

namespace hp {

struct StationheadPlayPeriodSummary {
  int64_t today = -1;
  int64_t yesterday = -1;
  int64_t thisWeek = -1;
  int64_t lastWeek = -1;
};

inline constexpr int64_t kStationheadSummaryDayMs = 24 * 60 * 60 * 1000;
inline constexpr int64_t kStationheadSummaryJstOffsetMs = 9 * 60 * 60 * 1000;

constexpr int64_t StationheadJstDayOrdinal(int64_t timestampMs) noexcept {
  return (timestampMs + kStationheadSummaryJstOffsetMs) / kStationheadSummaryDayMs;
}

template <typename Range>
constexpr StationheadPlayPeriodSummary SummarizeStationheadDailyPlays(
    const Range& points, int64_t nowMs) noexcept {
  const int64_t todayDay = StationheadJstDayOrdinal(nowMs);
  const int64_t mondayOffset = (todayDay + 3) % 7;
  std::array<int64_t, 14> daily{};
  daily.fill(-1);

  for (const auto& point : points) {
    if (point.dayStartMsUtc <= 0 || point.value < 0) continue;
    const int64_t ageDays = todayDay - StationheadJstDayOrdinal(point.dayStartMsUtc);
    if (ageDays < 0 || ageDays >= static_cast<int64_t>(daily.size())) continue;
    daily[static_cast<size_t>(ageDays)] = point.value;
  }

  StationheadPlayPeriodSummary summary;
  summary.today = daily[0];
  summary.yesterday = daily[1];

  int64_t total = 0;
  bool present = false;
  for (int64_t age = 0; age <= mondayOffset; ++age) {
    const int64_t value = daily[static_cast<size_t>(age)];
    if (value < 0) continue;
    total += value;
    present = true;
  }
  if (present) summary.thisWeek = total;

  total = 0;
  present = false;
  for (int64_t age = mondayOffset + 1; age <= mondayOffset + 7; ++age) {
    const int64_t value = daily[static_cast<size_t>(age)];
    if (value < 0) continue;
    total += value;
    present = true;
  }
  if (present) summary.lastWeek = total;
  return summary;
}

namespace stationhead_play_summary_checks {
inline constexpr std::array<StationheadDailyPlayPoint, 6> kFixture{{
    {1783296000000, 10},
    {1783814400000, 20},
    {1783900800000, 30},
    {1784332800000, 40},
    {1784419200000, 45},
    {1784419200000, 50},
}};
inline constexpr auto kSummary =
    SummarizeStationheadDailyPlays(kFixture, 1784462400000);
static_assert(kSummary.today == 50);
static_assert(kSummary.yesterday == 40);
static_assert(kSummary.thisWeek == 120);
static_assert(kSummary.lastWeek == 30);
}  // namespace stationhead_play_summary_checks

}  // namespace hp
