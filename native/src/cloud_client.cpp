#include "cloud_client.h"
#include "winhttp_helpers.h"
#include <winrt/Windows.Data.Json.h>
#include <iphlpapi.h>

namespace hp {
namespace {
constexpr size_t kMaxResponseBytes = 16 * 1024 * 1024;
constexpr UINT kStationheadHealthUpdatedMessage = WM_APP + 10;
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;
using winrt::Windows::Data::Json::JsonValueType;

std::wstring HeaderValue(HINTERNET request, DWORD query) {
  return QueryHeaderValue(request, query);
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
}

CloudClient::CloudClient(HWND window, AppConfig config, fs::path dataDir, std::wstring deviceToken,
                         std::wstring actionToken, Logger& log)
    : window_(window), config_(std::move(config)), dataDir_(std::move(dataDir)),
      deviceToken_(std::move(deviceToken)), actionToken_(std::move(actionToken)), log_(log) {
  if (actionToken_.empty()) actionToken_ = deviceToken_;
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
  StartNetworkChangeWatcher();
}

void CloudClient::Stop() {
  stopping_ = true;
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
  StopNetworkChangeWatcher();
}

void CloudClient::StartNetworkChangeWatcher() {



  networkChangeStopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!networkChangeStopEvent_) return;
  networkChangeThread_ = std::thread([this] {
    for (;;) {
      OVERLAPPED overlapped{};
      overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!overlapped.hEvent) return;
      HANDLE notifyHandle = nullptr;
      const DWORD requested = NotifyAddrChange(&notifyHandle, &overlapped);
      if (requested != ERROR_IO_PENDING) {
        CloseHandle(overlapped.hEvent);
        return;
      }

      HANDLE waitHandles[2] = {networkChangeStopEvent_, overlapped.hEvent};
      const DWORD wait = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
      if (wait == WAIT_OBJECT_0) {
        CancelIPChangeNotify(&overlapped);
        WaitForSingleObject(overlapped.hEvent, INFINITE);
        CloseHandle(overlapped.hEvent);
        return;
      }
      if (wait != WAIT_OBJECT_0 + 1) {
        CancelIPChangeNotify(&overlapped);
        WaitForSingleObject(overlapped.hEvent, INFINITE);
        CloseHandle(overlapped.hEvent);
        return;
      }
      CloseHandle(overlapped.hEvent);
      RefreshNow();
    }
  });
}

void CloudClient::StopNetworkChangeWatcher() {
  if (networkChangeStopEvent_) SetEvent(networkChangeStopEvent_);
  if (networkChangeThread_.joinable()) networkChangeThread_.join();
  if (networkChangeStopEvent_) {
    CloseHandle(networkChangeStopEvent_);
    networkChangeStopEvent_ = nullptr;
  }
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

void CloudClient::UpdateStationheadHealthText(std::wstring text) {
  bool changed = false;
  {
    std::lock_guard lock(stateMutex_);
    if (stationheadHealthText_ != text) {
      stationheadHealthText_ = std::move(text);
      changed = true;
    }
  }
  if (changed && window_) PostMessageW(window_, kStationheadHealthUpdatedMessage, 0, 0);
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
      UpdateStationheadHealthText(L"Stationhead収集: 状態取得失敗");
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
  if (actionToken_.empty()) {
    log_.Warn(L"Remote source refresh requires DEVICE_TOKEN or API_TOKEN; performing local sync only");
    RefreshNow();
    return false;
  }
  try {
    const auto response = Request(L"POST", L"/v1/refresh", actionToken_, {}, "{}");
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
}




#include "cloud_client_http.cpp"
#include "cloud_client_sync.cpp"
