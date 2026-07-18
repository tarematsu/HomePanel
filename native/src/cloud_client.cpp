#include "cloud_client.h"
#include "winhttp_helpers.h"
#include <charconv>
#include <limits>
#include <winrt/Windows.Data.Json.h>
#include <iphlpapi.h>

namespace hp {
namespace {
constexpr size_t kMaxResponseBytes = 16 * 1024 * 1024;
constexpr UINT kStationheadHealthUpdatedMessage = WM_APP + 10;
constexpr double kMaxJsonInteger = 9'007'199'254'740'991.0;
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

template <typename Integer>
bool AppendInteger(std::string& output, Integer value) {
  char buffer[32]{};
  const auto converted = std::to_chars(std::begin(buffer), std::end(buffer), value);
  if (converted.ec != std::errc{}) return false;
  output.append(buffer, converted.ptr);
  return true;
}

std::optional<std::string> ReadTextFile(const fs::path& path) {
  std::error_code error;
  const std::uintmax_t size = fs::file_size(path, error);
  if (error || size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
    return std::nullopt;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  std::string text(static_cast<size_t>(size), '\0');
  if (!text.empty()) {
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (input.gcount() != static_cast<std::streamsize>(text.size())) return std::nullopt;
  }
  if (input.peek() != std::char_traits<char>::eof()) return std::nullopt;
  return text;
}

std::wstring TelemetryUtf8BytesToWide(const std::vector<uint8_t>& bytes) {
  if (bytes.empty() ||
      bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return {};
  }
  const char* const input = reinterpret_cast<const char*>(bytes.data());
  const int inputSize = static_cast<int>(bytes.size());
  const int outputSize = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, input, inputSize, nullptr, 0);
  if (outputSize <= 0) return {};
  std::wstring output(static_cast<size_t>(outputSize), L'\0');
  if (MultiByteToWideChar(
          CP_UTF8, MB_ERR_INVALID_CHARS, input, inputSize,
          output.data(), outputSize) != outputSize) {
    return {};
  }
  return output;
}

std::vector<uint8_t> WideToUtf8Bytes(const wchar_t* input, size_t length) {
  if (!input || length == 0) return {};
  if (length > static_cast<size_t>(std::numeric_limits<int>::max())) return {};
  const int inputSize = static_cast<int>(length);
  const int outputSize = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, input, inputSize,
      nullptr, 0, nullptr, nullptr);
  if (outputSize <= 0) return {};
  std::vector<uint8_t> output(static_cast<size_t>(outputSize));
  if (WideCharToMultiByte(
          CP_UTF8, WC_ERR_INVALID_CHARS, input, inputSize,
          reinterpret_cast<char*>(output.data()), outputSize,
          nullptr, nullptr) != outputSize) {
    return {};
  }
  return output;
}

std::string EscapeJson(const std::wstring& value) {
  const std::string input = WideToUtf8(value);
  std::string output;
  output.reserve(input.size() + 8);
  for (char c : input) {
    if (c == '"' || c == '\\') {
      output.push_back('\\');
      output.push_back(c);
    } else if (c == '\n') {
      output.push_back('\\');
      output.push_back('n');
    } else if (static_cast<unsigned char>(c) >= 0x20) {
      output.push_back(c);
    }
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
    const std::optional<std::string> text = ReadTextFile(path);
    if (text && !text->empty()) root = JsonObject::Parse(Utf8ToWide(*text));
  } catch (...) {
    root = JsonObject{};
  }
  root.SetNamedValue(L"__cloudError", JsonValue::CreateStringValue(message));
  const winrt::hstring text = root.Stringify();
  return WideToUtf8Bytes(text.c_str(), text.size());
}

std::optional<std::vector<uint8_t>> StringPayload(const JsonObject& root, const wchar_t* name) {
  try {
    if (!root.HasKey(name) || root.GetNamedValue(name).ValueType() != JsonValueType::String) return std::nullopt;
    const winrt::hstring text = root.GetNamedString(name);
    return WideToUtf8Bytes(text.c_str(), text.size());
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
    const std::optional<std::string> text = ReadTextFile(dataDir_ / L"dashboard.meta.json");
    if (text && !text->empty()) {
      const JsonObject object = JsonObject::Parse(Utf8ToWide(*text));
      dashboardVersion_ = static_cast<int>(object.GetNamedNumber(L"dashboardVersion", -1));
      radarVersion_ = static_cast<int>(object.GetNamedNumber(L"radarVersion", -1));
      switchbotVersion_ = static_cast<int>(object.GetNamedNumber(L"switchbotVersion", -1));
      stationheadVersion_ = static_cast<int>(object.GetNamedNumber(L"stationheadVersion", -1));
      stationheadHealthVersion_ = static_cast<int>(object.GetNamedNumber(L"stationheadHealthVersion", -1));
      deviceConfigVersion_ = static_cast<int>(object.GetNamedNumber(L"deviceConfigVersion", -1));
    }
  } catch (...) {
    dashboardVersion_ = radarVersion_ = switchbotVersion_ = stationheadVersion_ =
        stationheadHealthVersion_ = deviceConfigVersion_ = -1;
  }
  cacheMetadataDirty_ = false;
}

void CloudClient::SaveCacheMetadata() {
  std::string text;
  text.reserve(192);
  text += "{\"dashboardVersion\":";
  const bool formatted = AppendInteger(text, dashboardVersion_);
  text += ",\"radarVersion\":";
  const bool radarFormatted = AppendInteger(text, radarVersion_);
  text += ",\"switchbotVersion\":";
  const bool switchbotFormatted = AppendInteger(text, switchbotVersion_);
  text += ",\"stationheadVersion\":";
  const bool stationheadFormatted = AppendInteger(text, stationheadVersion_);
  text += ",\"stationheadHealthVersion\":";
  const bool healthFormatted = AppendInteger(text, stationheadHealthVersion_);
  text += ",\"deviceConfigVersion\":";
  const bool configFormatted = AppendInteger(text, deviceConfigVersion_);
  text.push_back('}');
  if (!formatted || !radarFormatted || !switchbotFormatted ||
      !stationheadFormatted || !healthFormatted || !configFormatted) {
    log_.Warn(L"Cloud cache metadata formatting failed");
    return;
  }
  if (AtomicWriteText(dataDir_ / L"dashboard.meta.json", text)) cacheMetadataDirty_ = false;
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
    const size_t detailSize = std::min<size_t>(300, response.body.size());
    const std::string detail(
        reinterpret_cast<const char*>(response.body.data()), detailSize);
    throw std::runtime_error("update manifest HTTP " + std::to_string(response.status) + (detail.empty() ? "" : ": " + detail));
  }
  return std::string(response.body.begin(), response.body.end());
}

TelemetryReceipt CloudClient::SendTelemetry(const std::string& jsonBody) {
  try {
    const auto response = Request(L"POST", L"/v1/telemetry", deviceToken_, {}, jsonBody);
    if (response.status != 200) {
      log_.Warn(L"Telemetry returned HTTP " + std::to_wstring(response.status));
      return {};
    }

    const JsonObject object = JsonObject::Parse(TelemetryUtf8BytesToWide(response.body));
    if (!object.HasKey(L"acknowledgedSequences") ||
        object.GetNamedValue(L"acknowledgedSequences").ValueType() != JsonValueType::Array) {
      throw std::runtime_error("telemetry response omitted acknowledgedSequences");
    }
    const double nextValue = object.GetNamedNumber(L"nextSequence", -1);
    if (!std::isfinite(nextValue) || std::floor(nextValue) != nextValue ||
        nextValue <= 0 || nextValue > kMaxJsonInteger) {
      throw std::runtime_error("telemetry response contained invalid nextSequence");
    }

    const JsonArray acknowledgements = object.GetNamedArray(L"acknowledgedSequences");
    TelemetryReceipt receipt;
    receipt.nextSequence = static_cast<uint64_t>(nextValue);
    receipt.acknowledgedSequences.reserve(acknowledgements.Size());
    for (const auto& value : acknowledgements) {
      if (value.ValueType() != JsonValueType::Number) {
        throw std::runtime_error("telemetry response contained a non-numeric acknowledgement");
      }
      const double sequence = value.GetNumber();
      if (!std::isfinite(sequence) || std::floor(sequence) != sequence ||
          sequence <= 0 || sequence > kMaxJsonInteger) {
        throw std::runtime_error("telemetry response contained an invalid acknowledgement");
      }
      receipt.acknowledgedSequences.push_back(static_cast<uint64_t>(sequence));
    }
    std::sort(receipt.acknowledgedSequences.begin(), receipt.acknowledgedSequences.end());
    receipt.acknowledgedSequences.erase(
        std::unique(receipt.acknowledgedSequences.begin(), receipt.acknowledgedSequences.end()),
        receipt.acknowledgedSequences.end());
    receipt.success = true;
    return receipt;
  } catch (const std::exception& error) {
    log_.Warn(L"Telemetry failed: " + Utf8ToWide(error.what()));
  }
  return {};
}

bool CloudClient::AcknowledgeCommand(int64_t id, bool success, const std::wstring& result) {
  try {
    const std::string escapedResult = EscapeJson(result);
    std::string body;
    body.reserve(48 + escapedResult.size());
    body += "{\"id\":";
    if (!AppendInteger(body, id)) throw std::runtime_error("command acknowledgement id formatting failed");
    body += success ? ",\"success\":true,\"result\":\"" : ",\"success\":false,\"result\":\"";
    body += escapedResult;
    body += "\"}";
    const auto response = Request(L"POST", L"/v1/device/commands/ack?deviceId=" + config_.deviceId,
                                  deviceToken_, {}, body);
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
