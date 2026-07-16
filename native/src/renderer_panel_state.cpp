#include "web_renderer.h"

namespace hp {
namespace {
constexpr int64_t kAirGraphWindowMs = 24LL * 60 * 60 * 1000;
}

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
  const bool sensorsChanged = nativeSensors_ != state.sensors;
  const bool historyChanged = nativeAirHistory_ != state.airHistory;
  const bool stationheadChanged = nativeStationhead_ != state.stationhead;
  const bool stationheadHistoryChanged =
      nativeStationheadPlayHistory_ != state.stationheadPlayHistory;
  const bool controlsChanged =
      nativeAppVersion_ != state.appVersion || nativeToast_ != state.toast;
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
    RebuildNativeAirGraph(UnixMillis());
  }
  if (sensorsChanged || historyChanged) ++nativeAirRenderRevision_;
  if (stationheadHistoryChanged) {
    nativeStationheadPlayHistory_ = state.stationheadPlayHistory;
  }
  if (stationheadChanged) nativeStationhead_ = state.stationhead;
  if (controlsChanged) {
    nativeAppVersion_ = state.appVersion;
    nativeToast_ = state.toast;
  }
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
    InvalidatePanelSection(nativeMainWindow_, PanelSection::Air);
  }
  if (weatherChanged) {
    InvalidatePanelSection(nativeSideWindow_, PanelSection::Weather);
  }
  if (energyChanged) {
    InvalidatePanelSection(nativeSideWindow_, PanelSection::Energy);
  }
  if (newsChanged || newsIndexChanged) {
    InvalidatePanelSection(nativeMainWindow_, PanelSection::News);
  }
  if (controlsChanged) {
    InvalidatePanelSection(nativeSideWindow_, PanelSection::Controls);
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
    if (nativeMainWindow_ && IsWindow(nativeMainWindow_) &&
        IsWindowVisible(nativeMainWindow_)) {
      InvalidatePanelSection(nativeMainWindow_, PanelSection::Air);
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