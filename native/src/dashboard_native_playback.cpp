#include "web_renderer.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {
constexpr int64_t kNativePlaybackPollMs = 5 * 60'000;
constexpr size_t kMaxPlaybackResponseBytes = 4 * 1024 * 1024;

struct PlaybackEndpoint {
  const wchar_t* source;
  const wchar_t* url;
};

constexpr PlaybackEndpoint kPlaybackEndpoints[] = {
    {L"a", L"https://skrzk.pages.dev/api/playback?channel=buddies"},
    {L"b", L"https://skrzk.pages.dev/api/playback?channel=buddy46"},
};

struct NativeHttpHandle {
  HINTERNET value = nullptr;
  ~NativeHttpHandle() { if (value) WinHttpCloseHandle(value); }
  NativeHttpHandle() = default;
  explicit NativeHttpHandle(HINTERNET handle) : value(handle) {}
  NativeHttpHandle(const NativeHttpHandle&) = delete;
  NativeHttpHandle& operator=(const NativeHttpHandle&) = delete;
  operator HINTERNET() const noexcept { return value; }
};

std::wstring JsonQuote(const std::wstring& value) {
  std::wstring output = L"\"";
  for (wchar_t c : value) {
    switch (c) {
      case L'\\': output += L"\\\\"; break;
      case L'\"': output += L"\\\""; break;
      case L'\n': output += L"\\n"; break;
      case L'\r': break;
      case L'\t': output += L"\\t"; break;
      default:
        if (c >= 0x20) output.push_back(c);
        break;
    }
  }
  output.push_back(L'\"');
  return output;
}

std::wstring ShortError(const std::string& value) {
  std::wstring output = Utf8ToWide(value);
  if (output.size() > 160) output.resize(160);
  return output;
}

std::wstring LastWinHttpError(const char* stage) {
  std::ostringstream output;
  output << stage << " failed: " << GetLastError();
  return ShortError(output.str());
}

std::wstring RequestPathFromUrl(const URL_COMPONENTS& parts) {
  std::wstring path;
  if (parts.lpszUrlPath && parts.dwUrlPathLength) {
    path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
  }
  if (path.empty()) path = L"/";
  if (parts.lpszExtraInfo && parts.dwExtraInfoLength) {
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  }
  return path;
}

std::wstring FetchPlaybackJson(const wchar_t* rawUrl, std::wstring* payload) {
  if (!rawUrl || !*rawUrl || !payload) return L"playback URL missing";
  payload->clear();

  URL_COMPONENTS parts{sizeof(parts)};
  wchar_t host[256]{};
  wchar_t path[4096]{};
  wchar_t extra[2048]{};
  parts.lpszHostName = host;
  parts.dwHostNameLength = _countof(host);
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = _countof(path);
  parts.lpszExtraInfo = extra;
  parts.dwExtraInfoLength = _countof(extra);
  if (!WinHttpCrackUrl(rawUrl, 0, 0, &parts)) return LastWinHttpError("WinHttpCrackUrl");

  const std::wstring requestPath = RequestPathFromUrl(parts);

  // Tablets can sit on networks where WPAD/PAC autodetection never resolves,
  // so try automatic proxy detection first and fall back to a direct
  // connection instead of failing the poll outright.
  for (DWORD accessType : {WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_ACCESS_TYPE_NO_PROXY}) {
    NativeHttpHandle session(WinHttpOpen(
        L"HomePanel-Native-Playback/1.0",
        accessType,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session) {
      if (accessType == WINHTTP_ACCESS_TYPE_NO_PROXY) return LastWinHttpError("WinHttpOpen");
      continue;
    }
    DWORD protocols = WINHTTP_PROTOCOL_FLAG_HTTP2;
    WinHttpSetOption(session, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &protocols, sizeof(protocols));

    NativeHttpHandle connection(WinHttpConnect(
        session,
        std::wstring(host, parts.dwHostNameLength).c_str(),
        parts.nPort,
        0));
    if (!connection) {
      if (accessType == WINHTTP_ACCESS_TYPE_NO_PROXY) return LastWinHttpError("WinHttpConnect");
      continue;
    }

    NativeHttpHandle request(WinHttpOpenRequest(
        connection,
        L"GET",
        requestPath.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
      if (accessType == WINHTTP_ACCESS_TYPE_NO_PROXY) return LastWinHttpError("WinHttpOpenRequest");
      continue;
    }

    WinHttpSetTimeouts(request, 8000, 8000, 8000, 8000);
    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
    const wchar_t headers[] = L"Accept: application/json\r\nCache-Control: no-cache\r\nPragma: no-cache\r\n";
    if (!WinHttpSendRequest(
            request,
            headers,
            static_cast<DWORD>(wcslen(headers)),
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
      if (accessType == WINHTTP_ACCESS_TYPE_NO_PROXY) return LastWinHttpError("WinHttpReceiveResponse");
      continue;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &status,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
      return L"HTTP " + std::to_wstring(status);
    }

    std::string body;
    for (;;) {
      DWORD available = 0;
      if (!WinHttpQueryDataAvailable(request, &available)) return LastWinHttpError("WinHttpQueryDataAvailable");
      if (!available) break;
      if (body.size() + available > kMaxPlaybackResponseBytes) return L"playback response too large";
      const size_t offset = body.size();
      body.resize(offset + available);
      DWORD read = 0;
      if (!WinHttpReadData(request, body.data() + offset, available, &read)) return LastWinHttpError("WinHttpReadData");
      body.resize(offset + read);
      if (!read) break;
    }
    if (body.empty()) return L"empty playback response";

    const std::wstring wide = Utf8ToWide(body);
    if (wide.empty()) return L"invalid UTF-8 playback response";
    try {
      const auto value = winrt::Windows::Data::Json::JsonValue::Parse(wide);
      if (value.ValueType() != winrt::Windows::Data::Json::JsonValueType::Object) {
        return L"playback response is not a JSON object";
      }
    } catch (...) {
      return L"invalid playback JSON";
    }

    *payload = wide;
    return {};
  }
  return LastWinHttpError("WinHTTP native playback request");
}
}  // namespace

void Renderer::StartNativePlaybackBridge() {
  if (nativePlaybackStarted_.exchange(true, std::memory_order_acq_rel)) return;
  nativePlaybackStopping_ = false;
  nativePlaybackThread_ = std::thread([this] { NativePlaybackLoop(); });
}

void Renderer::StopNativePlaybackBridge() noexcept {
  if (!nativePlaybackStarted_.exchange(false, std::memory_order_acq_rel)) return;
  nativePlaybackStopping_ = true;
  nativePlaybackWake_.notify_all();
  if (nativePlaybackThread_.joinable()) nativePlaybackThread_.join();
}

void Renderer::NativePlaybackLoop() {
  while (!nativePlaybackStopping_.load(std::memory_order_acquire)) {
    for (size_t index = 0; index < std::size(kPlaybackEndpoints); ++index) {
      if (nativePlaybackStopping_.load(std::memory_order_acquire)) break;
      std::wstring payload;
      const std::wstring error = FetchPlaybackJson(kPlaybackEndpoints[index].url, &payload);
      {
        std::lock_guard lock(nativePlaybackMutex_);
        auto& update = nativePlaybackUpdates_[index];
        update.source = kPlaybackEndpoints[index].source;
        update.payload = std::move(payload);
        update.error = error;
        update.fetchedAt = UnixMillis();
        update.hasPayload = update.error.empty() && !update.payload.empty();
        update.revision = ++nativePlaybackRevision_;
      }
      InvalidateRect(window_, nullptr, FALSE);
    }

    std::unique_lock waitLock(nativePlaybackWakeMutex_);
    nativePlaybackWake_.wait_for(
        waitLock,
        std::chrono::milliseconds(kNativePlaybackPollMs),
        [this] { return nativePlaybackStopping_.load(std::memory_order_acquire); });
  }
}

void Renderer::FlushNativePlaybackMessages() {
  if (!ready_ || !uiReady_ || !webview_) return;

  std::array<NativePlaybackUpdate, 2> pending;
  std::array<bool, 2> hasPending{};
  {
    std::lock_guard lock(nativePlaybackMutex_);
    for (size_t index = 0; index < nativePlaybackUpdates_.size(); ++index) {
      if (nativePlaybackUpdates_[index].revision > nativePlaybackPostedRevisions_[index]) {
        pending[index] = nativePlaybackUpdates_[index];
        hasPending[index] = true;
      }
    }
  }

  for (size_t index = 0; index < pending.size(); ++index) {
    if (!hasPending[index]) continue;
    const auto& update = pending[index];
    std::wostringstream message;
    message << L"{\"type\":\"native-playback\""
            << L",\"source\":" << JsonQuote(update.source)
            << L",\"fetchedAt\":" << update.fetchedAt;
    if (update.hasPayload) message << L",\"payload\":" << update.payload;
    if (!update.error.empty()) message << L",\"error\":" << JsonQuote(update.error);
    message << L"}";

    if (SUCCEEDED(webview_->PostWebMessageAsJson(message.str().c_str()))) {
      std::lock_guard lock(nativePlaybackMutex_);
      nativePlaybackPostedRevisions_[index] = std::max(
          nativePlaybackPostedRevisions_[index], update.revision);
    }
  }
}
}  // namespace hp
