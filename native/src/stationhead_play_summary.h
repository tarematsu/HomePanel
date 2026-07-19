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

constexpr int64_t StationheadUtcDayOrdinal(int64_t timestampMs) noexcept {
  return timestampMs / kStationheadSummaryDayMs;
}

constexpr int64_t StationheadUtcMondayDayOrdinal(int64_t timestampMs) noexcept {
  const int64_t day = StationheadUtcDayOrdinal(timestampMs);
  // Unix day zero was Thursday, so adding three maps Monday to offset zero.
  const int64_t daysSinceMonday = (day + 3) % 7;
  return day - daysSinceMonday;
}

template <typename Range>
constexpr StationheadPlayPeriodSummary SummarizeStationheadDailyPlays(
    const Range& points, int64_t nowMs) noexcept {
  const int64_t todayDay = StationheadUtcDayOrdinal(nowMs);
  const int64_t thisWeekMonday = StationheadUtcMondayDayOrdinal(nowMs);
  const int64_t daysSinceThisWeekMonday = todayDay - thisWeekMonday;
  std::array<int64_t, 14> daily{};
  daily.fill(-1);

  for (const auto& point : points) {
    if (point.dayStartMsUtc <= 0 || point.value < 0) continue;
    const int64_t ageDays = todayDay - StationheadUtcDayOrdinal(point.dayStartMsUtc);
    if (ageDays < 0 || ageDays >= static_cast<int64_t>(daily.size())) continue;
    // The API can repeat a UTC date. The newest point wins without double-counting.
    daily[static_cast<size_t>(ageDays)] = point.value;
  }

  StationheadPlayPeriodSummary summary;
  summary.today = daily[0];
  summary.yesterday = daily[1];

  // This week is the current UTC calendar week: Monday through today.
  int64_t total = 0;
  bool present = false;
  for (int64_t age = 0; age <= daysSinceThisWeekMonday; ++age) {
    const int64_t value = daily[static_cast<size_t>(age)];
    if (value < 0) continue;
    total += value;
    present = true;
  }
  if (present) summary.thisWeek = total;

  // Last week is the complete preceding UTC calendar week: the Monday before
  // this week's Monday through the immediately preceding Sunday.
  total = 0;
  present = false;
  for (int64_t age = daysSinceThisWeekMonday + 1;
       age <= daysSinceThisWeekMonday + 7;
       ++age) {
    const int64_t value = daily[static_cast<size_t>(age)];
    if (value < 0) continue;
    total += value;
    present = true;
  }
  if (present) summary.lastWeek = total;
  return summary;
}

namespace stationhead_play_summary_checks {
inline constexpr std::array<StationheadDailyPlayPoint, 8> kFixture{{
    {1783209600000, 99},  // 2026-07-05 Sunday UTC: older than last week.
    {1783296000000, 10},  // 2026-07-06 Monday UTC: last week starts.
    {1783814400000, 20},  // 2026-07-12 Sunday UTC: last week ends.
    {1783900800000, 30},  // 2026-07-13 Monday UTC: this week starts.
    {1784332800000, 40},  // 2026-07-18 Saturday UTC.
    {1784419200000, 45},
    {1784419200000, 50},  // Repeated UTC date: newest value wins.
    {1784505600000, 5},   // 2026-07-20 Monday UTC.
}};

inline constexpr auto kSundaySummary =
    SummarizeStationheadDailyPlays(kFixture, 1784462400000);  // 2026-07-19 12:00 UTC.
static_assert(kSundaySummary.today == 50);
static_assert(kSundaySummary.yesterday == 40);
static_assert(kSundaySummary.thisWeek == 120);
static_assert(kSundaySummary.lastWeek == 30);

inline constexpr auto kMondaySummary =
    SummarizeStationheadDailyPlays(kFixture, 1783944000000);  // 2026-07-13 12:00 UTC.
static_assert(kMondaySummary.today == 30);
static_assert(kMondaySummary.yesterday == 20);
static_assert(kMondaySummary.thisWeek == 30);
static_assert(kMondaySummary.lastWeek == 30);

inline constexpr auto kBeforeJstNineSummary = SummarizeStationheadDailyPlays(
    kFixture, 1784505599999);  // 2026-07-20 08:59:59.999 JST; still July 19 UTC.
static_assert(kBeforeJstNineSummary.today == 50);
static_assert(kBeforeJstNineSummary.yesterday == 40);
static_assert(kBeforeJstNineSummary.thisWeek == 120);
static_assert(kBeforeJstNineSummary.lastWeek == 30);

inline constexpr auto kAtJstNineSummary = SummarizeStationheadDailyPlays(
    kFixture, 1784505600000);  // 2026-07-20 09:00 JST; July 20 UTC starts.
static_assert(kAtJstNineSummary.today == 5);
static_assert(kAtJstNineSummary.yesterday == 50);
static_assert(kAtJstNineSummary.thisWeek == 5);
static_assert(kAtJstNineSummary.lastWeek == 120);

static_assert(
    StationheadUtcMondayDayOrdinal(1783944000000) ==
    StationheadUtcDayOrdinal(1783900800000));
static_assert(
    StationheadUtcMondayDayOrdinal(1784462400000) ==
    StationheadUtcDayOrdinal(1783900800000));
static_assert(
    StationheadUtcDayOrdinal(1784505599999) ==
    StationheadUtcDayOrdinal(1784419200000));
static_assert(
    StationheadUtcDayOrdinal(1784505600000) ==
    StationheadUtcDayOrdinal(1784419200000) + 1);
}  // namespace stationhead_play_summary_checks

}  // namespace hp
