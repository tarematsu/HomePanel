#include "web_renderer.h"
#include "json_helpers.h"
#include "winhttp_helpers.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {
constexpr wchar_t kMinuteFactsUrl[] = L"https://skrzk.pages.dev/api/minute-facts/latest";
constexpr int64_t kMinuteFactsPollIntervalMs = 5 * 60'000;
constexpr size_t kMaxMinuteFactsResponseBytes = 1 * 1024 * 1024;

using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValueType;

NativeMinuteFactsProjection ParseMinuteFacts(const std::wstring& payload, int64_t fetchedAt) {
  NativeMinuteFactsProjection projection;
  projection.fetchedAt = fetchedAt;
  if (payload.empty()) return projection;
  try {
    const JsonObject root = JsonObject::Parse(payload);
    projection.ok = json::Boolean(root, L"ok");
    projection.stale = json::Boolean(root, L"stale");
    const JsonArray rows = json::Array(root, L"rows");
    if (rows.Size() > 0 && rows.GetAt(0).ValueType() == JsonValueType::Object) {
      const JsonObject row = rows.GetAt(0).GetObject();
      projection.isBroadcasting = json::Number(row, L"is_broadcasting") != 0;
      projection.isPaused = json::Number(row, L"is_paused") != 0;
      projection.listenerCount = static_cast<int>(json::Number(row, L"listener_count"));
      projection.onlineMemberCount = static_cast<int>(json::Number(row, L"online_member_count"));
      projection.minuteAt = static_cast<int64_t>(json::Number(row, L"minute_at"));
      projection.available = true;
    }
  } catch (...) {
    NativeMinuteFactsProjection failed;
    failed.fetchedAt = fetchedAt;
    return failed;
  }
  return projection;
}

std::wstring FetchMinuteFactsJson(std::wstring* payload) {
  if (!payload) return L"minute facts payload missing";
  payload->clear();

  std::wstring requestUrl(kMinuteFactsUrl);
  requestUrl += L"?_hp=" + std::to_wstring(UnixMillis());

  std::vector<uint8_t> body;
  std::wstring error;
  if (!WinHttpDownload(requestUrl, kMaxMinuteFactsResponseBytes, &body, nullptr, &error,
                       L"HomePanel-Native-MinuteFacts/1.0",
                       L"Accept: application/json\r\nCache-Control: no-cache, no-store\r\nPragma: no-cache\r\n")) {
    return error.empty() ? L"minute facts download failed" : error;
  }

  const std::wstring wide = Utf8ToWide(std::string(body.begin(), body.end()));
  if (wide.empty()) return L"invalid UTF-8 minute facts response";
  *payload = wide;
  return {};
}
}  // namespace

void Renderer::StartNativeMinuteFactsBridge() {
  if (nativeMinuteFactsStarted_.exchange(true, std::memory_order_acq_rel)) return;
  nativeMinuteFactsStopping_ = false;
  nativeMinuteFactsThread_ = std::thread([this] { NativeMinuteFactsLoop(); });
}

void Renderer::StopNativeMinuteFactsBridge() noexcept {
  if (!nativeMinuteFactsStarted_.exchange(false, std::memory_order_acq_rel)) return;
  nativeMinuteFactsStopping_ = true;
  nativeMinuteFactsWake_.notify_all();
  if (nativeMinuteFactsThread_.joinable()) nativeMinuteFactsThread_.join();
}

void Renderer::NativeMinuteFactsLoop() {
  const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  while (!nativeMinuteFactsStopping_.load(std::memory_order_acquire)) {
    std::wstring payload;
    const int64_t fetchedAt = UnixMillis();
    const std::wstring error = FetchMinuteFactsJson(&payload);
    if (error.empty()) {
      NativeMinuteFactsProjection projection = ParseMinuteFacts(payload, fetchedAt);
      if (projection.available) {
        std::lock_guard lock(nativeMinuteFactsMutex_);
        nativeMinuteFacts_ = projection;
      }
      InvalidatePanelSection(nativeMainWindow_, PanelSection::Music);
    }

    std::unique_lock waitLock(nativeMinuteFactsWakeMutex_);
    nativeMinuteFactsWake_.wait_for(
        waitLock, std::chrono::milliseconds(kMinuteFactsPollIntervalMs),
        [this] { return nativeMinuteFactsStopping_.load(std::memory_order_acquire); });
  }
  if (SUCCEEDED(apartment)) CoUninitialize();
}

NativeMinuteFactsProjection Renderer::NativeMinuteFactsSnapshot() const {
  std::lock_guard lock(nativeMinuteFactsMutex_);
  return nativeMinuteFacts_;
}

}  // namespace hp
