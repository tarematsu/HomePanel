// Part of cloud_client.cpp's translation unit (see the #include at the end of
// that file). Device sync: the home-presence fallback, radar-tile localization
// into the local cache, and the main /v1/device/sync exchange that refreshes
// dashboard/radar/switchbot/stationhead/config and dispatches commands. Uses
// the JSON usings and VersionOr/StringPayload/IsoLocalNow helpers from
// cloud_client.cpp.
#include "cloud_client.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {

void CloudClient::ApplyPresenceFallback() {
  if (presenceFallbackActive_) return;
  JsonObject fallback;
  try {
    std::ifstream input(dataDir_ / L"switchbot.json", std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(input)), {});
    if (!text.empty()) fallback = JsonObject::Parse(Utf8ToWide(text));
  } catch (...) {
    fallback = JsonObject{};
  }
  fallback.SetNamedValue(L"presence", JsonValue::CreateStringValue(L"home"));
  fallback.SetNamedValue(L"fallback", JsonValue::CreateBooleanValue(true));
  fallback.SetNamedValue(L"fallbackReason", JsonValue::CreateStringValue(L"external-service-unavailable"));
  const std::string text = WideToUtf8(fallback.Stringify().c_str());
  const std::vector<uint8_t> bytes(text.begin(), text.end());
  if (!AtomicWriteBytes(dataDir_ / L"switchbot.json", bytes)) {
    log_.Warn(L"Failed to write home-presence fallback state");
    return;
  }
  switchbotVersion_ = -1;
  cacheMetadataDirty_ = true;
  presenceFallbackActive_ = true;
  PostMessageW(window_, WM_HP_SWITCHBOT_UPDATED, 0, 0);
  log_.Warn(L"External service unavailable; presence forced to home until a fresh SwitchBot state is received");
}

std::vector<uint8_t> CloudClient::LocalizeRadarTiles(const std::vector<uint8_t>& body) {
  const auto localPathFor = [this](const std::wstring& url) -> fs::path {
    fs::path path = dataDir_ / L"radar-cache";
    size_t start = 0;
    while (start < url.size() && url[start] == L'/') ++start;
    while (start < url.size()) {
      const size_t slash = url.find(L'/', start);
      const std::wstring part = url.substr(start, slash == std::wstring::npos ? std::wstring::npos : slash - start);
      if (part.empty() || part == L"." || part == L"..") throw std::runtime_error("invalid radar tile path");
      path /= part;
      if (slash == std::wstring::npos) break;
      start = slash + 1;
    }
    return path;
  };
  const auto localUrlFor = [](const std::wstring& url) {
    return L"https://data.homepanel/radar-cache" + (url.empty() || url.front() == L'/' ? url : L"/" + url);
  };
  const auto remoteUrlFor = [this](const std::wstring& url) {
    std::wstring base = config_.cloudflareBaseUrl;
    while (!base.empty() && base.back() == L'/') base.pop_back();
    return base + (url.empty() || url.front() == L'/' ? url : L"/" + url);
  };
  const auto localizeTile = [&](JsonObject item) {
    const std::wstring url = item.GetNamedString(L"url", L"").c_str();
    if (url.empty() || url.front() != L'/') return;
    const fs::path target = localPathFor(url);
    std::error_code error;
    if (!fs::exists(target, error) || fs::file_size(target, error) == 0) {
      const auto response = Request(L"GET", url, deviceToken_);
      if (response.status != 200 || response.body.empty()) {
        log_.Warn(L"Radar tile cache fetch failed; using remote tile URL: HTTP " +
                  std::to_wstring(response.status));
        item.SetNamedValue(L"url", JsonValue::CreateStringValue(remoteUrlFor(url)));
        return;
      }
      if (!AtomicWriteBytes(target, response.body)) {
        log_.Warn(L"Radar tile cache write failed; using remote tile URL");
        item.SetNamedValue(L"url", JsonValue::CreateStringValue(remoteUrlFor(url)));
        return;
      }
    }
    item.SetNamedValue(L"url", JsonValue::CreateStringValue(localUrlFor(url)));
  };

  JsonObject root = JsonObject::Parse(Utf8ToWide(std::string(body.begin(), body.end())));
  if (root.HasKey(L"baseTiles") && root.GetNamedValue(L"baseTiles").ValueType() == JsonValueType::Array) {
    for (auto value : root.GetNamedArray(L"baseTiles")) {
      if (value.ValueType() == JsonValueType::Object) localizeTile(value.GetObject());
    }
  }
  if (root.HasKey(L"frames") && root.GetNamedValue(L"frames").ValueType() == JsonValueType::Array) {
    for (auto frameValue : root.GetNamedArray(L"frames")) {
      if (frameValue.ValueType() != JsonValueType::Object) continue;
      const JsonObject frame = frameValue.GetObject();
      if (!frame.HasKey(L"tiles") || frame.GetNamedValue(L"tiles").ValueType() != JsonValueType::Array) continue;
      for (auto tileValue : frame.GetNamedArray(L"tiles")) {
        if (tileValue.ValueType() == JsonValueType::Object) localizeTile(tileValue.GetObject());
      }
    }
  }
  const std::string text = WideToUtf8(root.Stringify().c_str());
  return {text.begin(), text.end()};
}

void CloudClient::Synchronize() {
  if (config_.cloudflareBaseUrl.empty()) throw std::runtime_error("cloudflareBaseUrl is empty");
  if (deviceToken_.empty()) throw std::runtime_error("device token missing");

  const fs::path dashboardPath = dataDir_ / L"dashboard.json";
  const fs::path radarPath = dataDir_ / L"radar.json";
  const fs::path switchbotPath = dataDir_ / L"switchbot.json";
  const fs::path stationheadPath = dataDir_ / L"stationhead.json";
  const fs::path deviceConfigPath = dataDir_ / L"device-config.json";
  const auto requestedVersion = [](const fs::path& path, int version) {
    std::error_code error;
    return fs::exists(path, error) ? version : -1;
  };

  std::wostringstream path;
  path << L"/v1/device/sync?deviceId=" << config_.deviceId
       << L"&dashboardVersion=" << requestedVersion(dashboardPath, dashboardVersion_)
       << L"&radarVersion=" << requestedVersion(radarPath, radarVersion_)
       << L"&switchbotVersion=" << (presenceFallbackActive_ ? -1 : requestedVersion(switchbotPath, switchbotVersion_))
       << L"&stationheadVersion=" << requestedVersion(stationheadPath, stationheadVersion_)
       << L"&configVersion=" << requestedVersion(deviceConfigPath, deviceConfigVersion_);

  const auto response = Request(L"GET", path.str(), deviceToken_);
  if (response.status != 200) throw std::runtime_error("device sync HTTP " + std::to_string(response.status));
  const JsonObject root = JsonObject::Parse(Utf8ToWide(std::string(response.body.begin(), response.body.end())));
  const JsonObject versions = root.GetNamedObject(L"versions", JsonObject{});

  const int nextDashboard = VersionOr(versions, L"dashboard", dashboardVersion_);
  const int nextRadar = VersionOr(versions, L"radar", radarVersion_);
  const int nextSwitchbot = VersionOr(versions, L"switchbot", switchbotVersion_);
  const int nextStationhead = VersionOr(versions, L"stationhead", stationheadVersion_);
  const int nextConfig = VersionOr(versions, L"config", deviceConfigVersion_);

  if (auto payload = StringPayload(root, L"dashboard")) {
    if (!AtomicWriteBytes(dashboardPath, *payload)) throw std::runtime_error("dashboard data cache write failed");
    PostMessageW(window_, WM_HP_CLOUD_UPDATED, 0, 0);
  }
  if (auto payload = StringPayload(root, L"radar")) {
    if (!AtomicWriteBytes(radarPath, LocalizeRadarTiles(*payload))) throw std::runtime_error("radar cache write failed");
    PostMessageW(window_, WM_HP_RADAR_UPDATED, 0, 0);
  }
  if (auto payload = StringPayload(root, L"switchbot")) {
    if (!AtomicWriteBytes(switchbotPath, *payload)) throw std::runtime_error("SwitchBot cache write failed");
    presenceFallbackActive_ = false;
    PostMessageW(window_, WM_HP_SWITCHBOT_UPDATED, 0, 0);
  }
  if (auto payload = StringPayload(root, L"stationhead")) {
    if (!AtomicWriteBytes(stationheadPath, *payload)) throw std::runtime_error("Stationhead state cache write failed");
    PostMessageW(window_, WM_HP_STATIONHEAD_CHANGED, 0, 0);
  }
  if (auto payload = StringPayload(root, L"deviceConfig")) {
    if (!AtomicWriteBytes(deviceConfigPath, *payload)) throw std::runtime_error("device config cache write failed");
    PostMessageW(window_, WM_HP_CONFIG_UPDATED, 0, 0);
  }

  if (root.HasKey(L"commands") && root.GetNamedValue(L"commands").ValueType() == JsonValueType::Array) {
    const JsonArray commands = root.GetNamedArray(L"commands");
    if (commands.Size() > 0) {
      JsonObject envelope;
      envelope.SetNamedValue(L"deviceId", JsonValue::CreateStringValue(config_.deviceId));
      envelope.SetNamedValue(L"commands", commands);
      const std::string text = WideToUtf8(envelope.Stringify().c_str());
      if (!AtomicWriteBytes(dataDir_ / L"commands.json", {text.begin(), text.end()})) {
        throw std::runtime_error("device command cache write failed");
      }
      PostMessageW(window_, WM_HP_COMMANDS_UPDATED, 0, 0);
    }
  }

  if (dashboardVersion_ != nextDashboard || radarVersion_ != nextRadar ||
      switchbotVersion_ != nextSwitchbot || stationheadVersion_ != nextStationhead ||
      deviceConfigVersion_ != nextConfig) {
    dashboardVersion_ = nextDashboard;
    radarVersion_ = nextRadar;
    switchbotVersion_ = nextSwitchbot;
    stationheadVersion_ = nextStationhead;
    deviceConfigVersion_ = nextConfig;
    cacheMetadataDirty_ = true;
  }
  if (cacheMetadataDirty_) SaveCacheMetadata();
  failures_ = 0;
  {
    std::lock_guard lock(stateMutex_);
    lastSuccess_ = IsoLocalNow();
    workerVersion_ = root.GetNamedString(L"workerVersion", L"").c_str();
  }
}

}  // namespace hp
