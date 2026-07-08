#include "web_renderer.h"

namespace hp {
namespace statejson {
std::wstring Sensors(const SensorSnapshot& value);
std::wstring Player(const StationheadStatus& value);
}
namespace {
constexpr uint32_t kDashboardSlice = 1u << 0;
constexpr uint32_t kAirHistorySlice = 1u << 1;
constexpr uint32_t kSensorsSlice = 1u << 2;
constexpr uint32_t kStationheadSlice = 1u << 3;
constexpr uint32_t kSpotifySlice = 1u << 4;
constexpr uint32_t kAppVersionScalar = 1u << 19;
constexpr uint32_t kAllSlices = kDashboardSlice | kAirHistorySlice | kSensorsSlice |
                                   kStationheadSlice | kSpotifySlice;
constexpr uint32_t kWorkspaceScalar = 1u << 16;
constexpr uint32_t kNewsIndexScalar = 1u << 17;
constexpr uint32_t kToastScalar = 1u << 18;
constexpr uint32_t kAllScalars = kWorkspaceScalar | kNewsIndexScalar | kToastScalar |
                                 kAppVersionScalar;
constexpr uint32_t kAllStateFields = kAllSlices | kAllScalars;

std::wstring Quote(const std::wstring& value) {
  std::wstring output = L"\"";
  for (wchar_t c : value) {
    if (c == L'\\' || c == L'\"') output.push_back(L'\\');
    if (c == L'\n') output += L"\\n";
    else if (c != L'\r') output.push_back(c);
  }
  output.push_back(L'\"');
  return output;
}

bool SameSensors(const SensorSnapshot& left, const SensorSnapshot& right) {
  return left.co2Connected == right.co2Connected &&
         left.co2 == right.co2 &&
         left.temperatureCorrected == right.temperatureCorrected &&
         left.humidityCorrected == right.humidityCorrected &&
         left.presence == right.presence &&
         left.light == right.light &&
         left.motion == right.motion &&
         left.doorOpen == right.doorOpen &&
         left.outboxCount == right.outboxCount &&
         left.lastError == right.lastError;
}

bool SameStationhead(const StationheadStatus& left, const StationheadStatus& right) {
  return left.created == right.created &&
         left.navigating == right.navigating &&
         left.playing == right.playing &&
         left.audioPlaying == right.audioPlaying &&
         left.loginRequired == right.loginRequired &&
         left.lightweight == right.lightweight &&
         left.visible == right.visible &&
         left.processFailed == right.processFailed &&
         left.spotifyConfigured == right.spotifyConfigured &&
         left.authAvailable == right.authAvailable &&
         left.audioSilent == right.audioSilent &&
         left.sampledAt == right.sampledAt &&
         left.expectedEndAt == right.expectedEndAt &&
         left.trackDurationMs == right.trackDurationMs &&
         left.detail == right.detail &&
         left.trackTitle == right.trackTitle &&
         left.trackArtist == right.trackArtist &&
         left.deviceName == right.deviceName &&
         left.artworkUrl == right.artworkUrl &&
         left.url == right.url;
}
}

std::wstring Renderer::AirHistoryJson(const std::vector<AirHistorySample>& history) const {
  std::wostringstream json;
  json << L"[";
  bool first = true;
  for (const auto& sample : history) {
    if (sample.timestamp <= 0 || sample.co2 < 250 || sample.co2 > 10000 ||
        sample.humidity < 0 || sample.humidity > 100 ||
        sample.temperature < -40 || sample.temperature > 85) continue;
    if (!first) json << L",";
    first = false;
    json << L"{\"t\":" << sample.timestamp
         << L",\"co2\":" << sample.co2
         << L",\"temperature\":" << sample.temperature
         << L",\"humidity\":" << sample.humidity << L"}";
  }
  json << L"]";
  return json.str();
}

std::wstring Renderer::BuildCachedStateJson(uint32_t changedFields, bool full) const {
  if (!stateJsonCache_.initialized) return {};
  const auto include = [changedFields, full](uint32_t field) {
    return full || (changedFields & field) != 0;
  };

  std::wostringstream json;
  json << L"{\"type\":\"state\""
       << L",\"full\":" << (full ? L"true" : L"false");

  if (full || (changedFields & (kAllSlices | kNewsIndexScalar)) != 0) {
    json << L",\"revisions\":{";
    bool firstRevision = true;
    const auto appendRevision = [&](const wchar_t* name, uint64_t value) {
      if (!firstRevision) json << L",";
      firstRevision = false;
      json << L"\"" << name << L"\":" << value;
    };
    if (include(kDashboardSlice)) appendRevision(L"dashboard", stateJsonCache_.dashboardRevision);
    if (include(kAirHistorySlice)) appendRevision(L"airHistory", stateJsonCache_.airHistoryRevision);
    if (include(kSensorsSlice)) appendRevision(L"sensors", stateJsonCache_.sensorsRevision);
    if (include(kStationheadSlice)) appendRevision(L"stationhead", stateJsonCache_.stationheadRevision);
    if (include(kSpotifySlice)) appendRevision(L"spotify", stateJsonCache_.spotifyRevision);
    if (include(kAppVersionScalar)) appendRevision(L"appVersion", stateJsonCache_.appVersionRevision);
    if (include(kNewsIndexScalar)) appendRevision(L"news", stateJsonCache_.newsRevision);
    json << L"}";
  }

  if (include(kWorkspaceScalar)) json << L",\"workspaceTab\":" << stateJsonCache_.workspaceTab;
  if (include(kNewsIndexScalar)) json << L",\"newsIndex\":" << stateJsonCache_.newsIndex;
  if (include(kToastScalar)) json << L",\"toast\":" << Quote(stateJsonCache_.toast);
  if (include(kAppVersionScalar)) json << L",\"appVersion\":" << Quote(stateJsonCache_.appVersion);
  if (include(kDashboardSlice)) json << L",\"dashboard\":" << stateJsonCache_.dashboard;
  if (include(kSpotifySlice)) json << L",\"spotify\":" << stateJsonCache_.spotify;
  if (include(kAirHistorySlice)) json << L",\"airHistory\":" << stateJsonCache_.airHistory;
  if (include(kSensorsSlice)) json << L",\"sensors\":" << stateJsonCache_.sensors;
  if (include(kStationheadSlice)) json << L",\"stationhead\":" << stateJsonCache_.stationhead;
  json << L"}";
  return json.str();
}
}  // namespace hp
