#include "web_renderer.h"

namespace hp {
namespace {
constexpr int64_t kAirGraphWindowMs = 24LL * 60 * 60 * 1000;

struct NativeHistoryRevisionCache {
  const Renderer* owner = nullptr;
  uint64_t air = 0;
  uint64_t stationhead = 0;
};

NativeHistoryRevisionCache& HistoryRevisionCacheFor(const Renderer* owner) {
  static NativeHistoryRevisionCache cache;
  if (cache.owner != owner) {
    cache.owner = owner;
    cache.air = std::numeric_limits<uint64_t>::max();
    cache.stationhead = std::numeric_limits<uint64_t>::max();
  }
  return cache;
}
}  // namespace

void Renderer::RebuildNativeAirGraph(int64_t nowMs) {
  AirGraphProjection next;
  next.cutoff = nowMs - kAirGraphWindowMs;
  next.samples.reserve(nativeAirHistory_.size());
  double co2Min = std::numeric_limits<double>::max();
  double co2Max = std::numeric_limits<double>::lowest();
  double temperatureMin = std::numeric_limits<double>::max();
  double temperatureMax = std::numeric_limits<double>::lowest();
  double humidityMin = std::numeric_limits<double>::max();
  double humidityMax = std::numeric_limits<double>::lowest();
  for (const auto& sample : nativeAirHistory_) {
    if (sample.timestamp < next.cutoff || sample.co2 < 250 || sample.co2 > 10000 ||
        sample.temperature < -40 || sample.temperature > 85 ||
        sample.humidity < 0 || sample.humidity > 100) {
      continue;
    }
    next.samples.push_back(sample);
    co2Min = std::min(co2Min, static_cast<double>(sample.co2));
    co2Max = std::max(co2Max, static_cast<double>(sample.co2));
    temperatureMin = std::min(temperatureMin, sample.temperature);
    temperatureMax = std::max(temperatureMax, sample.temperature);
    humidityMin = std::min(humidityMin, sample.humidity);
    humidityMax = std::max(humidityMax, sample.humidity);
  }
  if (!next.samples.empty()) {
    next.co2Min = co2Min;
    next.co2Max = co2Max;
    next.temperatureMin = temperatureMin;
    next.temperatureMax = temperatureMax;
    next.humidityMin = humidityMin;
    next.humidityMax = humidityMax;
  }
  nativeAirGraph_ = std::move(next);
}

void Renderer::UpdateNativeStaticPanels(const RenderState& state) {
  NativeHistoryRevisionCache& historyRevisions = HistoryRevisionCacheFor(this);
  const bool sensorsChanged = nativeSensors_ != state.sensors;
  const bool historyChanged = historyRevisions.air != state.airHistoryRevision;
  const bool stationheadChanged =
      nativeStationhead_.contentRevision != state.stationhead.contentRevision ||
      nativeStationhead_.secondaryContentRevision != state.stationhead.secondaryContentRevision ||
      nativeStationhead_.audioMuted != state.stationhead.audioMuted ||
      nativeStationhead_.secondaryAudioMuted != state.stationhead.secondaryAudioMuted ||
      nativeStationhead_.primaryAudioSelected != state.stationhead.primaryAudioSelected;
  const bool stationheadHistoryChanged =
      historyRevisions.stationhead != state.stationheadPlayHistoryRevision;
  const bool newsIndexChanged = nativeNewsIndex_ != state.newsIndex;
  const bool weatherChanged =
      renderedDashboardRevisions_.weather != dashboardRevisions_.weather;
  const bool energyChanged =
      renderedDashboardRevisions_.energy != dashboardRevisions_.energy;
  const bool newsChanged =
      renderedDashboardRevisions_.news != dashboardRevisions_.news;

  if (sensorsChanged) nativeSensors_ = state.sensors;
  if (historyChanged) {
    nativeAirHistory_ = state.airHistory;
    historyRevisions.air = state.airHistoryRevision;
    RebuildNativeAirGraph(UnixMillis());
  }
  if (sensorsChanged || historyChanged) ++nativeAirRenderRevision_;
  if (stationheadHistoryChanged) {
    nativeStationheadPlayHistory_ = state.stationheadPlayHistory;
    historyRevisions.stationhead = state.stationheadPlayHistoryRevision;
  }
  if (stationheadChanged) nativeStationhead_ = state.stationhead;
  if (newsIndexChanged) nativeNewsIndex_ = state.newsIndex;
  if (weatherChanged) {
    renderedDashboardRevisions_.weather = dashboardRevisions_.weather;
  }
  if (energyChanged) {
    renderedDashboardRevisions_.energy = dashboardRevisions_.energy;
  }
  if (newsChanged) {
    renderedDashboardRevisions_.news = dashboardRevisions_.news;
  }
  if (newsChanged || newsIndexChanged) ++nativeNewsRenderRevision_;

  if (!EnsureNativeStaticWindows()) return;
  if (sensorsChanged || historyChanged) {
    InvalidatePanelSection(nativeSideWindow_, PanelSection::Air);
  }
  if (weatherChanged) {
    InvalidatePanelSection(nativeSideWindow_, PanelSection::Weather);
  }
  if (energyChanged) {
    InvalidatePanelSection(nativeMainWindow_, PanelSection::Energy);
  }
  if (newsChanged || newsIndexChanged) {
    InvalidatePanelSection(nativeMainWindow_, PanelSection::News);
  }
  if (stationheadChanged || stationheadHistoryChanged) {
    InvalidatePanelSection(nativeMainWindow_, PanelSection::Music);
  }
}

void Renderer::TickNativePanels(int64_t nowMs, bool timerDriven) {
  if (!nativeDashboardVisible_ || (!timerDriven && nativePanelTimerActive_)) return;

  const int64_t airCutoff = nowMs - kAirGraphWindowMs;
  const bool airGraphExpired = !nativeAirGraph_.samples.empty() &&
      nativeAirGraph_.samples.front().timestamp < airCutoff;
  if (airGraphExpired) {
    RebuildNativeAirGraph(nowMs);
    ++nativeAirRenderRevision_;
    if (nativeSideWindow_ && IsWindow(nativeSideWindow_) &&
        IsWindowVisible(nativeSideWindow_)) {
      InvalidatePanelSection(nativeSideWindow_, PanelSection::Air);
    }
  }

  SYSTEMTIME localTime{};
  GetLocalTime(&localTime);
  const int clockDayKey = static_cast<int>(localTime.wYear) * 10'000 +
      static_cast<int>(localTime.wMonth) * 100 + static_cast<int>(localTime.wDay);
  const bool clockDayChanged = clockDayKey != nativeClockDayKey_;
  nativeClockDayKey_ = clockDayKey;

  const NativePlaybackTickState playbackState = NativePlaybackTickStateFor(nowMs);
  const bool playbackChanged = playbackState != nativePlaybackTickState_;
  nativePlaybackTickState_ = playbackState;
  if (nativeSideWindow_ && IsWindow(nativeSideWindow_) &&
      IsWindowVisible(nativeSideWindow_)) {
    InvalidatePanelSection(nativeSideWindow_,
                           clockDayChanged ? PanelSection::Clock : PanelSection::ClockTime);
  }
  if (nativeMainWindow_ && IsWindow(nativeMainWindow_) &&
      IsWindowVisible(nativeMainWindow_)) {
    if (playbackChanged) {
      InvalidatePanelSection(nativeMainWindow_, PanelSection::Music);
    } else if (playbackState.active) {
      InvalidatePanelSection(nativeMainWindow_, PanelSection::PlaybackProgress);
    }
  }
}

}  // namespace hp
