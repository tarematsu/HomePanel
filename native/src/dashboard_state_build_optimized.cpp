#include "web_renderer.h"

namespace hp {
std::wstring Renderer::BuildStateJson(const RenderState& state, bool full) {
  const bool firstState = !stateJsonCache_.initialized;
  uint32_t changedFields = 0;

  if (firstState || stateJsonCache_.dashboardSourceRevision != dashboardSourceRevision_) {
    stateJsonCache_.dashboard = dashboardJson_.empty() ? L"{}" : dashboardJson_;
    stateJsonCache_.dashboardSourceRevision = dashboardSourceRevision_;
    ++stateJsonCache_.dashboardRevision;
    changedFields |= kDashboardSlice;
  }

  if (firstState || stateJsonCache_.spotifySourceRevision != spotifySourceRevision_) {
    stateJsonCache_.spotify = spotifyJson_.empty() ? L"{}" : spotifyJson_;
    stateJsonCache_.spotifySourceRevision = spotifySourceRevision_;
    ++stateJsonCache_.spotifyRevision;
    changedFields |= kSpotifySlice;
  }

  const int64_t airHistoryLastT =
      state.airHistory.empty() ? 0 : state.airHistory.back().timestamp;
  if (firstState || stateJsonCache_.airHistorySourceCount != state.airHistory.size() ||
      stateJsonCache_.airHistorySourceLastT != airHistoryLastT) {
    stateJsonCache_.airHistory = AirHistoryJson(state.airHistory);
    stateJsonCache_.airHistorySourceCount = state.airHistory.size();
    stateJsonCache_.airHistorySourceLastT = airHistoryLastT;
    ++stateJsonCache_.airHistoryRevision;
    changedFields |= kAirHistorySlice;
  }

  if (firstState || !SameSensors(stateJsonCache_.sensorsSource, state.sensors)) {
    stateJsonCache_.sensorsSource = state.sensors;
    stateJsonCache_.sensors = statejson::Sensors(state.sensors);
    ++stateJsonCache_.sensorsRevision;
    changedFields |= kSensorsSlice;
  }

  if (firstState ||
      !SameStationhead(stateJsonCache_.stationheadSource, state.stationhead)) {
    stateJsonCache_.stationheadSource = state.stationhead;
    stateJsonCache_.stationhead = statejson::Player(state.stationhead);
    ++stateJsonCache_.stationheadRevision;
    changedFields |= kStationheadSlice;
  }

  if (firstState || stateJsonCache_.workspaceTab != state.workspaceTab) {
    changedFields |= kWorkspaceScalar;
  }
  if (firstState || stateJsonCache_.newsIndex != state.newsIndex) {
    changedFields |= kNewsIndexScalar;
    ++stateJsonCache_.newsRevision;
  }
  if (firstState || stateJsonCache_.toast != state.toast) {
    changedFields |= kToastScalar;
  }
  if (firstState || stateJsonCache_.appVersion != state.appVersion) {
    stateJsonCache_.appVersion = state.appVersion;
    ++stateJsonCache_.appVersionRevision;
    changedFields |= kAppVersionScalar;
  }

  stateJsonCache_.workspaceTab = state.workspaceTab;
  stateJsonCache_.newsIndex = state.newsIndex;
  stateJsonCache_.toast = state.toast;
  stateJsonCache_.initialized = true;

  if (!full && changedFields == 0) return {};
  return BuildCachedStateJson(firstState ? kAllStateFields : changedFields, full);
}
}  // namespace hp
