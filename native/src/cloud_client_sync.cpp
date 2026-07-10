





#include "cloud_client.h"
#include <set>
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {
int64_t HealthNumber(const JsonObject& root, const wchar_t* name) {
  try {
    const double value = root.GetNamedNumber(name, -1);
    return std::isfinite(value) && value >= 0 ? static_cast<int64_t>(value) : -1;
  } catch (...) {
    return -1;
  }
}

std::wstring HealthTimeText(int64_t timestampMs) {
  if (timestampMs < 0) return L"--:--";
  const std::time_t seconds = static_cast<std::time_t>(timestampMs / 1000);
  std::tm local{};
  if (localtime_s(&local, &seconds) != 0) return L"--:--";
  wchar_t text[16]{};
  swprintf_s(text, L"%02d:%02d", local.tm_hour, local.tm_min);
  return text;
}

std::wstring StationheadHealthSummary(const JsonObject& root) {
  const bool configured = root.GetNamedBoolean(L"configured", false);
  if (!configured) return L"Stationhead収集: 未設定";

  const bool reachable = root.GetNamedBoolean(L"reachable", false);
  const bool healthy = root.GetNamedBoolean(L"healthy", false);
  const int64_t lastSuccessAt = HealthNumber(root, L"lastSuccessAt");
  const std::wstring lastSuccess = HealthTimeText(lastSuccessAt);
  if (!reachable) return L"Stationhead収集: 状態取得失敗";

  std::wstring result = healthy
      ? L"Stationhead収集: 稼働中"
      : L"Stationhead収集: 停止";
  result += L"  最終成功 " + lastSuccess;

  if (lastSuccessAt >= 0) {
    const int64_t ageMinutes = std::max<int64_t>(0, (UnixMillis() - lastSuccessAt) / 60'000);
    if (ageMinutes < 24 * 60) result += L" (" + std::to_wstring(ageMinutes) + L"分前)";
  }
  return result;
}
}  // namespace

std::wstring CloudClient::StationheadHealthText() const {
  std::lock_guard lock(stateMutex_);
  return stationheadHealthText_;
}

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
  const fs::path cacheRoot = dataDir_ / L"radar-cache";
  const auto pathOnly = [](const std::wstring& url) {
    const size_t query = url.find_first_of(L"?#");
    return url.substr(0, query);
  };
  const auto localPathFor = [&](const std::wstring& url) -> fs::path {
    const std::wstring pathname = pathOnly(url);
    fs::path path = cacheRoot;
    size_t start = 0;
    while (start < pathname.size() && pathname[start] == L'/') ++start;
    while (start < pathname.size()) {
      const size_t slash = pathname.find(L'/', start);
      const std::wstring part = pathname.substr(
          start, slash == std::wstring::npos ? std::wstring::npos : slash - start);
      if (part.empty() || part == L"." || part == L".." ||
          part.find_first_of(L"\\:*?\"<>|") != std::wstring::npos) {
        throw std::runtime_error("invalid radar tile path");
      }
      path /= part;
      if (slash == std::wstring::npos) break;
      start = slash + 1;
    }
    return path.lexically_normal();
  };
  const auto localUrlFor = [&](const std::wstring& url) {
    const std::wstring pathname = pathOnly(url);
    return L"https://data.homepanel/radar-cache" +
        (pathname.empty() || pathname.front() == L'/' ? pathname : L"/" + pathname);
  };
  const auto remoteUrlFor = [this](const std::wstring& url) {
    std::wstring base = config_.cloudflareBaseUrl;
    while (!base.empty() && base.back() == L'/') base.pop_back();
    return base + (url.empty() || url.front() == L'/' ? url : L"/" + url);
  };

  std::set<std::wstring> retained;
  const auto localizeTile = [&](JsonObject item) {
    const std::wstring url = item.GetNamedString(L"url", L"").c_str();
    if (url.empty() || url.front() != L'/') return;
    const fs::path target = localPathFor(url);
    retained.insert(target.wstring());
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

  if (!retained.empty()) {
    std::vector<fs::path> staleFiles;
    std::vector<fs::path> directories;
    std::error_code walkError;
    fs::recursive_directory_iterator iterator(
        cacheRoot, fs::directory_options::skip_permission_denied, walkError);
    const fs::recursive_directory_iterator end;
    while (!walkError && iterator != end) {
      const fs::path current = iterator->path().lexically_normal();
      std::error_code itemError;
      if (iterator->is_regular_file(itemError) && !retained.contains(current.wstring())) {
        staleFiles.push_back(current);
      } else if (!itemError && iterator->is_directory(itemError)) {
        directories.push_back(current);
      }
      iterator.increment(walkError);
    }
    for (const auto& stale : staleFiles) {
      std::error_code ignored;
      fs::remove(stale, ignored);
    }
    std::sort(directories.begin(), directories.end(), [](const fs::path& left, const fs::path& right) {
      return left.native().size() > right.native().size();
    });
    for (const auto& directory : directories) {
      std::error_code ignored;
      fs::remove(directory, ignored);
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

  bool dashboardApplied = false;
  bool radarApplied = false;
  bool switchbotApplied = false;
  bool stationheadApplied = false;
  bool configApplied = false;

  if (auto payload = StringPayload(root, L"dashboard")) {
    if (!AtomicWriteBytes(dashboardPath, *payload)) throw std::runtime_error("dashboard data cache write failed");
    dashboardApplied = true;
    PostMessageW(window_, WM_HP_CLOUD_UPDATED, 0, 0);
  }
  if (auto payload = StringPayload(root, L"radar")) {
    if (!AtomicWriteBytes(radarPath, LocalizeRadarTiles(*payload))) throw std::runtime_error("radar cache write failed");
    radarApplied = true;
    PostMessageW(window_, WM_HP_RADAR_UPDATED, 0, 0);
  }
  if (auto payload = StringPayload(root, L"switchbot")) {
    if (!AtomicWriteBytes(switchbotPath, *payload)) throw std::runtime_error("SwitchBot cache write failed");
    switchbotApplied = true;
    presenceFallbackActive_ = false;
    PostMessageW(window_, WM_HP_SWITCHBOT_UPDATED, 0, 0);
  }
  if (auto payload = StringPayload(root, L"stationhead")) {
    if (!AtomicWriteBytes(stationheadPath, *payload)) throw std::runtime_error("Stationhead state cache write failed");
    stationheadApplied = true;
    PostMessageW(window_, WM_HP_STATIONHEAD_CHANGED, 0, 0);
  }
  if (auto payload = StringPayload(root, L"deviceConfig")) {
    if (!AtomicWriteBytes(deviceConfigPath, *payload)) throw std::runtime_error("device config cache write failed");
    configApplied = true;
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

  const auto acceptedVersion = [this](const wchar_t* name, int current, int next, bool payloadApplied) {
    if (next == current || payloadApplied) return next;
    log_.Warn(std::wstring(L"Cloud sync withheld ") + name +
              L" version " + std::to_wstring(next) +
              L" because its payload was absent");
    return current;
  };
  const int acceptedDashboard = acceptedVersion(L"dashboard", dashboardVersion_, nextDashboard, dashboardApplied);
  const int acceptedRadar = acceptedVersion(L"radar", radarVersion_, nextRadar, radarApplied);
  const int acceptedSwitchbot = acceptedVersion(L"switchbot", switchbotVersion_, nextSwitchbot, switchbotApplied);
  const int acceptedStationhead = acceptedVersion(L"stationhead", stationheadVersion_, nextStationhead, stationheadApplied);
  const int acceptedConfig = acceptedVersion(L"device config", deviceConfigVersion_, nextConfig, configApplied);

  if (dashboardVersion_ != acceptedDashboard || radarVersion_ != acceptedRadar ||
      switchbotVersion_ != acceptedSwitchbot || stationheadVersion_ != acceptedStationhead ||
      deviceConfigVersion_ != acceptedConfig) {
    dashboardVersion_ = acceptedDashboard;
    radarVersion_ = acceptedRadar;
    switchbotVersion_ = acceptedSwitchbot;
    stationheadVersion_ = acceptedStationhead;
    deviceConfigVersion_ = acceptedConfig;
    cacheMetadataDirty_ = true;
  }
  if (cacheMetadataDirty_) SaveCacheMetadata();

  std::wstring nextHealthText;
  try {
    const HttpResponse healthResponse = Request(L"GET", L"/v1/stationhead-health", deviceToken_);
    if (healthResponse.status == 200) {
      const std::string healthBody(healthResponse.body.begin(), healthResponse.body.end());
      nextHealthText = StationheadHealthSummary(JsonObject::Parse(Utf8ToWide(healthBody)));
    } else {
      nextHealthText = L"Stationhead収集: 状態取得失敗 (HTTP " +
          std::to_wstring(healthResponse.status) + L")";
    }
  } catch (const std::exception& error) {
    log_.Warn(L"Stationhead health read failed without interrupting dashboard sync: " + Utf8ToWide(error.what()));
    nextHealthText = L"Stationhead収集: 状態取得失敗";
  } catch (...) {
    log_.Warn(L"Stationhead health read failed without interrupting dashboard sync");
    nextHealthText = L"Stationhead収集: 状態取得失敗";
  }
  UpdateStationheadHealthText(std::move(nextHealthText));
  {
    std::lock_guard lock(stateMutex_);
    lastSuccess_ = IsoLocalNow();
    workerVersion_ = root.GetNamedString(L"workerVersion", L"").c_str();
  }
  failures_ = 0;
}

}  // namespace hp
