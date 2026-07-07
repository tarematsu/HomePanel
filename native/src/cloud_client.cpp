#include "cloud_client.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {
constexpr size_t kMaxResponseBytes = 16 * 1024 * 1024;
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;
using winrt::Windows::Data::Json::JsonValueType;

std::wstring HeaderValue(HINTERNET request, DWORD query) {
  DWORD size = 0;
  WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size, WINHTTP_NO_HEADER_INDEX);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !size) return {};
  std::wstring value(size / sizeof(wchar_t), L'\0');
  if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size, WINHTTP_NO_HEADER_INDEX)) return {};
  value.resize(wcsnlen(value.c_str(), value.size()));
  return value;
}

std::wstring IsoLocalNow() {
  SYSTEMTIME time{};
  GetLocalTime(&time);
  wchar_t buffer[64]{};
  swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
  return buffer;
}

std::string EscapeJson(const std::wstring& value) {
  std::string input = WideToUtf8(value), output;
  output.reserve(input.size());
  for (char c : input) {
    if (c == '"' || c == '\\') { output.push_back('\\'); output.push_back(c); }
    else if (c == '\n') output += "\\n";
    else if (static_cast<unsigned char>(c) >= 0x20) output.push_back(c);
  }
  return output;
}

std::wstring FriendlyCloudError(const std::string& raw) {
  if (raw.find("device token missing") != std::string::npos) return L"認証情報なし: C:\\HomePanel\\.env の DEVICE_TOKEN を確認";
  if (raw.find("HTTP 401") != std::string::npos) return L"認証エラー: DEVICE_TOKENが一致しません (HTTP 401)";
  if (raw.find("HTTP 403") != std::string::npos) return L"権限エラー: DEVICE_TOKEN / API_TOKENを確認 (HTTP 403)";
  if (raw.find("HTTP 404") != std::string::npos) return L"APIエラー: 本番Workerが旧版か、cloudflareBaseUrlが別Workerです (HTTP 404)";
  if (raw.find("WinHTTP") != std::string::npos || raw.find("WinHttp") != std::string::npos) return L"通信エラー: Cloudflareへ接続できません";
  if (raw.find("cloudflareBaseUrl") != std::string::npos || raw.find("Cloudflare base URL") != std::string::npos) return L"設定エラー: Cloudflare URLを確認してください";
  std::wstring detail = Utf8ToWide(raw);
  if (detail.size() > 64) detail.resize(64);
  return L"クラウドエラー: " + detail;
}

std::vector<uint8_t> DashboardWithCloudError(const fs::path& path, const std::wstring& message) {
  JsonObject root;
  try {
    std::ifstream input(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    if (!text.empty()) root = JsonObject::Parse(Utf8ToWide(text));
  } catch (...) {
    root = JsonObject{};
  }
  root.SetNamedValue(L"__cloudError", JsonValue::CreateStringValue(message));
  const std::string text = WideToUtf8(root.Stringify().c_str());
  return {text.begin(), text.end()};
}

std::optional<std::vector<uint8_t>> StringPayload(const JsonObject& root, const wchar_t* name) {
  try {
    if (!root.HasKey(name) || root.GetNamedValue(name).ValueType() != JsonValueType::String) return std::nullopt;
    const std::string text = WideToUtf8(root.GetNamedString(name).c_str());
    return std::vector<uint8_t>(text.begin(), text.end());
  } catch (...) {
    return std::nullopt;
  }
}

int VersionOr(const JsonObject& versions, const wchar_t* name, int fallback) {
  try {
    const double value = versions.GetNamedNumber(name, fallback);
    return std::isfinite(value) ? static_cast<int>(value) : fallback;
  } catch (...) {
    return fallback;
  }
}

std::string WinHttpErrorText(const char* action) {
  return std::string(action) + " failed (" + std::to_string(GetLastError()) + ")";
}
}  // namespace

CloudClient::CloudClient(HWND window, AppConfig config, fs::path dataDir, std::wstring deviceToken,
                         std::wstring actionToken, Logger& log)
    : window_(window), config_(std::move(config)), dataDir_(std::move(dataDir)),
      deviceToken_(std::move(deviceToken)), actionToken_(std::move(actionToken)), log_(log) {
  fs::create_directories(dataDir_);
  LoadCacheMetadata();
}

CloudClient::~CloudClient() {
  Stop();
  std::lock_guard lock(httpMutex_);
  ResetHttpHandlesLocked();
}

void CloudClient::Start() {
  stopping_ = false;
  thread_ = std::thread([this] { Loop(); });
}

void CloudClient::Stop() {
  stopping_ = true;
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void CloudClient::RefreshNow() {
  immediate_ = true;
  wake_.notify_all();
}

std::wstring CloudClient::LastSuccessText() const {
  std::lock_guard lock(stateMutex_);
  return lastSuccess_;
}

std::wstring CloudClient::WorkerVersion() const {
  std::lock_guard lock(stateMutex_);
  return workerVersion_;
}

void CloudClient::ResetHttpHandlesLocked() {
  if (connection_) WinHttpCloseHandle(connection_);
  if (session_) WinHttpCloseHandle(session_);
  connection_ = nullptr;
  session_ = nullptr;
  host_.clear();
  basePath_.clear();
  port_ = 0;
}

void CloudClient::EnsureHttpHandlesLocked() {
  if (session_ && connection_) return;
  if (config_.cloudflareBaseUrl.empty()) throw std::runtime_error("cloudflareBaseUrl is empty");

  URL_COMPONENTS parts{sizeof(parts)};
  wchar_t host[256]{};
  wchar_t path[2048]{};
  parts.lpszHostName = host;
  parts.dwHostNameLength = _countof(host);
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = _countof(path);
  if (!WinHttpCrackUrl(config_.cloudflareBaseUrl.c_str(), 0, 0, &parts)) throw std::runtime_error("invalid Cloudflare base URL");

  host_.assign(host, parts.dwHostNameLength);
  basePath_.assign(path, parts.dwUrlPathLength);
  while (!basePath_.empty() && basePath_.back() == L'/') basePath_.pop_back();
  port_ = parts.nPort;
  secure_ = parts.nScheme == INTERNET_SCHEME_HTTPS;

  session_ = WinHttpOpen(L"HomePanel/2.6", accessType_,
                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session_) throw std::runtime_error(WinHttpErrorText("WinHttpOpen"));
  DWORD protocols = WINHTTP_PROTOCOL_FLAG_HTTP2;
  WinHttpSetOption(session_, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &protocols, sizeof(protocols));
  connection_ = WinHttpConnect(session_, host_.c_str(), port_, 0);
  if (!connection_) {
    ResetHttpHandlesLocked();
    throw std::runtime_error(WinHttpErrorText("WinHttpConnect"));
  }
}

HttpResponse CloudClient::Request(const std::wstring& method, const std::wstring& path, const std::wstring& token,
                                  const std::wstring& etag, const std::string& body, const wchar_t* contentType) {
  std::lock_guard lock(httpMutex_);
  for (int attempt = 0; attempt < 2; ++attempt) {
    EnsureHttpHandlesLocked();
    const std::wstring requestPath = basePath_ + path;
    HINTERNET request = WinHttpOpenRequest(connection_, method.c_str(), requestPath.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           secure_ ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
      ResetHttpHandlesLocked();
      if (accessType_ == WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY) accessType_ = WINHTTP_ACCESS_TYPE_NO_PROXY;
      if (!attempt) continue;
      throw std::runtime_error(WinHttpErrorText("WinHttpOpenRequest"));
    }

    int timeout = 8000;
    WinHttpSetTimeouts(request, timeout, timeout, timeout, timeout);
    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
    std::wstring headers = L"Accept-Encoding: gzip\r\n";
    if (!token.empty()) headers += L"Authorization: Bearer " + token + L"\r\n";
    if (!etag.empty()) headers += L"If-None-Match: " + etag + L"\r\n";
    if (!body.empty()) headers += std::wstring(L"Content-Type: ") + contentType + L"\r\n";

    const BOOL sent = WinHttpSendRequest(
        request, headers.c_str(), static_cast<DWORD>(headers.size()),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
      const DWORD requestError = GetLastError();
      WinHttpCloseHandle(request);
      ResetHttpHandlesLocked();
      if (accessType_ == WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY) {
        accessType_ = WINHTTP_ACCESS_TYPE_NO_PROXY;
        log_.Warn(L"WinHTTP automatic proxy failed; retrying without proxy");
      }
      if (!attempt) continue;
      throw std::runtime_error("WinHTTP request failed (" + std::to_string(requestError) + ")");
    }

    HttpResponse output;
    DWORD statusSize = sizeof(output.status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &output.status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    output.etag = HeaderValue(request, WINHTTP_QUERY_ETAG);
    bool readOk = true;
    bool tooLarge = false;
    for (;;) {
      DWORD available = 0;
      if (!WinHttpQueryDataAvailable(request, &available)) { readOk = false; break; }
      if (!available) break;
      if (output.body.size() + available > kMaxResponseBytes) { tooLarge = true; break; }
      const size_t offset = output.body.size();
      output.body.resize(offset + available);
      DWORD read = 0;
      if (!WinHttpReadData(request, output.body.data() + offset, available, &read)) { readOk = false; break; }
      output.body.resize(offset + read);
      if (!read) break;
    }
    WinHttpCloseHandle(request);
    if (tooLarge) throw std::runtime_error("Cloudflare response exceeded 16 MiB");
    if (!readOk) throw std::runtime_error(WinHttpErrorText("WinHTTP response read"));
    return output;
  }
  throw std::runtime_error("WinHTTP request failed");
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
  const auto localizeTile = [&](JsonObject item) {
    const std::wstring url = item.GetNamedString(L"url", L"").c_str();
    if (url.empty() || url.front() != L'/') return;
    const fs::path target = localPathFor(url);
    std::error_code error;
    if (!fs::exists(target, error) || fs::file_size(target, error) == 0) {
      const auto response = Request(L"GET", url, deviceToken_);
      if (response.status != 200 || response.body.empty()) {
        throw std::runtime_error("radar tile fetch HTTP " + std::to_string(response.status));
      }
      if (!AtomicWriteBytes(target, response.body)) throw std::runtime_error("radar tile cache write failed");
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

void CloudClient::LoadCacheMetadata() {
  try {
    std::ifstream input(dataDir_ / L"dashboard.meta.json", std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(input)), {});
    if (!text.empty()) {
      const JsonObject object = JsonObject::Parse(Utf8ToWide(text));
      dashboardVersion_ = static_cast<int>(object.GetNamedNumber(L"dashboardVersion", -1));
      radarVersion_ = static_cast<int>(object.GetNamedNumber(L"radarVersion", -1));
      switchbotVersion_ = static_cast<int>(object.GetNamedNumber(L"switchbotVersion", -1));
      stationheadVersion_ = static_cast<int>(object.GetNamedNumber(L"stationheadVersion", -1));
      deviceConfigVersion_ = static_cast<int>(object.GetNamedNumber(L"deviceConfigVersion", -1));
    }
  } catch (...) {
    dashboardVersion_ = radarVersion_ = switchbotVersion_ = stationheadVersion_ = deviceConfigVersion_ = -1;
  }
  cacheMetadataDirty_ = false;
}

void CloudClient::SaveCacheMetadata() {
  std::ostringstream out;
  out << "{\"dashboardVersion\":" << dashboardVersion_
      << ",\"radarVersion\":" << radarVersion_
      << ",\"switchbotVersion\":" << switchbotVersion_
      << ",\"stationheadVersion\":" << stationheadVersion_
      << ",\"deviceConfigVersion\":" << deviceConfigVersion_ << "}";
  const std::string text = out.str();
  if (AtomicWriteBytes(dataDir_ / L"dashboard.meta.json", {text.begin(), text.end()})) cacheMetadataDirty_ = false;
  else log_.Warn(L"Cloud cache metadata write failed");
}

void CloudClient::Loop() {
  while (!stopping_) {
    try { Synchronize(); }
    catch (const std::exception& error) {
      ApplyPresenceFallback();
      dashboardVersion_ = -1;
      cacheMetadataDirty_ = true;
      const std::wstring friendly = FriendlyCloudError(error.what());
      const auto dashboard = DashboardWithCloudError(dataDir_ / L"dashboard.json", friendly);
      if (AtomicWriteBytes(dataDir_ / L"dashboard.json", dashboard)) PostMessageW(window_, WM_HP_CLOUD_UPDATED, 0, 0);
      const int count = ++failures_;
      log_.Warn(L"Cloudflare sync failed (" + std::to_wstring(count) + L"): " + Utf8ToWide(error.what()));
    }
    const int failures = failures_.load();
    const int seconds = failures == 0 ? config_.cloudPollSeconds : failures == 1 ? 120 : failures == 2 ? 300 : 900;
    std::unique_lock lock(wakeMutex_);
    wake_.wait_for(lock, std::chrono::seconds(seconds), [this] { return stopping_.load() || immediate_.exchange(false); });
  }
}

bool CloudClient::RequestRemoteRefresh() {
  try {
    const auto response = Request(L"POST", L"/v1/refresh", actionToken_.empty() ? deviceToken_ : actionToken_, {}, "{}");
    if (response.status == 202) { RefreshNow(); return true; }
    log_.Warn(L"Refresh request returned HTTP " + std::to_wstring(response.status));
  } catch (const std::exception& error) {
    log_.Warn(L"Refresh request failed: " + Utf8ToWide(error.what()));
  }
  return false;
}

std::string CloudClient::FetchUpdateManifest() {
  const auto response = Request(L"GET", L"/v1/update/manifest", deviceToken_);
  if (response.status != 200) {
    std::string detail(response.body.begin(), response.body.end());
    if (detail.size() > 300) detail.resize(300);
    throw std::runtime_error("update manifest HTTP " + std::to_string(response.status) + (detail.empty() ? "" : ": " + detail));
  }
  return std::string(response.body.begin(), response.body.end());
}

bool CloudClient::SendTelemetry(const std::string& jsonBody) {
  try {
    const auto response = Request(L"POST", L"/v1/telemetry", deviceToken_, {}, jsonBody);
    if (response.status == 200) return true;
    log_.Warn(L"Telemetry returned HTTP " + std::to_wstring(response.status));
  } catch (const std::exception& error) {
    log_.Warn(L"Telemetry failed: " + Utf8ToWide(error.what()));
  }
  return false;
}

bool CloudClient::AcknowledgeCommand(int64_t id, bool success, const std::wstring& result) {
  try {
    std::ostringstream body;
    body << "{\"id\":" << id << ",\"success\":" << (success ? "true" : "false")
         << ",\"result\":\"" << EscapeJson(result) << "\"}";
    const auto response = Request(L"POST", L"/v1/device/commands/ack?deviceId=" + config_.deviceId,
                                  deviceToken_, {}, body.str());
    if (response.status == 200) return true;
    log_.Warn(L"Command acknowledgement returned HTTP " + std::to_wstring(response.status));
  } catch (const std::exception& error) {
    log_.Warn(L"Command acknowledgement failed: " + Utf8ToWide(error.what()));
  }
  return false;
}
}  // namespace hp
