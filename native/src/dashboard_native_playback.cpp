#include "web_renderer.h"
#include "artwork_cache.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {
constexpr int64_t kNativePlaybackPollMs = 60 * 60'000;
constexpr size_t kMaxPlaybackResponseBytes = 4 * 1024 * 1024;
constexpr int64_t kPlaybackTransitionHoldMs = 1'000;
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;
using winrt::Windows::Data::Json::JsonValueType;

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

JsonObject AsObject(const JsonObject& parent, const wchar_t* name) {
  try {
    if (parent.HasKey(name) && parent.GetNamedValue(name).ValueType() == JsonValueType::Object) {
      return parent.GetNamedObject(name);
    }
  } catch (...) {
  }
  return JsonObject{};
}

JsonArray AsArray(const JsonObject& parent, const wchar_t* name) {
  try {
    if (parent.HasKey(name) && parent.GetNamedValue(name).ValueType() == JsonValueType::Array) {
      return parent.GetNamedArray(name);
    }
  } catch (...) {
  }
  return JsonArray{};
}

std::wstring StringValue(const JsonObject& object, const wchar_t* name) {
  try {
    if (object.HasKey(name) && object.GetNamedValue(name).ValueType() == JsonValueType::String) {
      return object.GetNamedString(name).c_str();
    }
  } catch (...) {
  }
  return {};
}

double NumberValue(const JsonObject& object, const wchar_t* name, double fallback = 0) {
  try {
    if (object.HasKey(name) && object.GetNamedValue(name).ValueType() == JsonValueType::Number) {
      return object.GetNamedNumber(name);
    }
  } catch (...) {
  }
  return fallback;
}

bool BoolValue(const JsonObject& object, const wchar_t* name, bool fallback = false) {
  try {
    if (object.HasKey(name) &&
        object.GetNamedValue(name).ValueType() == JsonValueType::Boolean) {
      return object.GetNamedBoolean(name);
    }
  } catch (...) {
  }
  return fallback;
}

double FirstNumber(const JsonObject& object, std::initializer_list<const wchar_t*> names) {
  for (const wchar_t* name : names) {
    const double value = NumberValue(object, name, std::numeric_limits<double>::quiet_NaN());
    if (std::isfinite(value)) return value;
  }
  return 0;
}

std::wstring FirstText(const JsonObject& object, std::initializer_list<const wchar_t*> names) {
  for (const wchar_t* name : names) {
    std::wstring value = StringValue(object, name);
    if (!value.empty()) return value;
  }
  return {};
}

JsonObject NormalizeItem(const fs::path& dataDir, const JsonObject& raw) {
  JsonObject source = raw;
  JsonObject nested = AsObject(raw, L"track");
  if (nested.Size() == 0) nested = AsObject(raw, L"song");
  if (nested.Size() != 0) source = nested;

  JsonObject item;
  item.SetNamedValue(L"title", JsonValue::CreateStringValue(
      FirstText(source, {L"name", L"title", L"trackTitle"})));

  std::wstring artist = FirstText(source, {L"trackArtist", L"artist"});
  if (artist.empty()) {
    const JsonArray artists = AsArray(source, L"artists");
    std::wstring joined;
    for (uint32_t index = 0; index < artists.Size(); ++index) {
      try {
        std::wstring part;
        const auto value = artists.GetAt(index);
        if (value.ValueType() == JsonValueType::String) part = value.GetString().c_str();
        else if (value.ValueType() == JsonValueType::Object) {
          part = StringValue(value.GetObject(), L"name");
        }
        if (part.empty()) continue;
        if (!joined.empty()) joined += L", ";
        joined += part;
      } catch (...) {
      }
    }
    artist = joined;
  }
  item.SetNamedValue(L"artist", JsonValue::CreateStringValue(artist));

  std::wstring artwork = FirstText(source, {
      L"artwork", L"artworkUrl", L"albumArtUrl", L"image", L"imageUrl",
      L"thumbnail_url",
  });
  if (artwork.empty()) {
    JsonObject album = AsObject(source, L"album");
    JsonArray images = AsArray(album, L"images");
    if (images.Size() > 0) {
      try {
        if (images.GetAt(0).ValueType() == JsonValueType::Object) {
          artwork = StringValue(images.GetAt(0).GetObject(), L"url");
        }
      } catch (...) {
      }
    }
  }
  item.SetNamedValue(L"artwork", JsonValue::CreateStringValue(CacheArtworkUrl(dataDir, artwork)));
  item.SetNamedValue(L"durationMs", JsonValue::CreateNumberValue(std::max(
      0.0, FirstNumber(source, {L"durationMs", L"duration_ms", L"lengthMs"}))));
  return item;
}

std::wstring ResolvePlaybackPayload(const fs::path& dataDir, const std::wstring& payload, int64_t fetchedAt) {
  if (payload.empty()) return L"{}";
  try {
    JsonObject root = JsonObject::Parse(payload);
    JsonObject value = root;
    for (const wchar_t* name : {L"playback", L"data", L"stationhead", L"result"}) {
      JsonObject child = AsObject(value, name);
      if (child.Size() != 0) {
        value = child;
        if (name == std::wstring(L"data")) {
          JsonObject nested = AsObject(value, L"playback");
          if (nested.Size() != 0) value = nested;
        }
        break;
      }
    }

    const bool playing = BoolValue(value, L"playing") ||
        (BoolValue(value, L"is_broadcasting") && !BoolValue(value, L"is_paused"));
    const int64_t sampledAt = static_cast<int64_t>(std::max(
        0.0, FirstNumber(value, {L"sampledAt", L"monitorSampledAt", L"updatedAt"})));
    const int64_t anchorAt = static_cast<int64_t>(std::max(0.0, NumberValue(value, L"anchorAt")));
    const int64_t queueEndAt = static_cast<int64_t>(std::max(0.0, NumberValue(value, L"queueEndAt")));
    int64_t durationMs = static_cast<int64_t>(std::max(
        0.0, FirstNumber(value, {L"durationMs", L"trackDurationMs"})));
    int64_t progressMs = static_cast<int64_t>(std::max(
        0.0, FirstNumber(value, {L"progressMs", L"positionMs"})));

    JsonArray queue = AsArray(value, L"queue");
    JsonObject itemSource = AsObject(value, L"item");
    if (itemSource.Size() == 0) itemSource = AsObject(value, L"currentItem");
    if (itemSource.Size() == 0) itemSource = AsObject(value, L"currentTrack");
    if (itemSource.Size() == 0) itemSource = AsObject(value, L"track");

    if (queue.Size() > 0) {
      int index = static_cast<int>(FirstNumber(value, {L"currentIndex", L"current_index"}));
      JsonObject queueStatus = AsObject(value, L"queue_status");
      if (index < 0) index = static_cast<int>(FirstNumber(queueStatus, {L"currentIndex", L"current_index"}));
      if (index < 0 || index >= static_cast<int>(queue.Size())) {
        index = 0;
        for (uint32_t queueIndex = 0; queueIndex < queue.Size(); ++queueIndex) {
          try {
            if (queue.GetAt(queueIndex).ValueType() == JsonValueType::Object &&
                BoolValue(queue.GetAt(queueIndex).GetObject(), L"is_current")) {
              index = static_cast<int>(queueIndex);
              break;
            }
          } catch (...) {
          }
        }
      }

      int64_t elapsed = progressMs;
      if (playing) {
        if (anchorAt > 0) elapsed = std::max<int64_t>(0, fetchedAt - anchorAt);
        else if (sampledAt > 0) elapsed += std::max<int64_t>(0, fetchedAt - sampledAt);
      }

      while (index >= 0 && index < static_cast<int>(queue.Size())) {
        if (queue.GetAt(index).ValueType() != JsonValueType::Object) break;
        itemSource = queue.GetAt(index).GetObject();
        JsonObject normalized = NormalizeItem(dataDir, itemSource);
        const int64_t queueDuration = static_cast<int64_t>(NumberValue(normalized, L"durationMs"));
        if (!playing || queueDuration <= 0 || elapsed < queueDuration + kPlaybackTransitionHoldMs) {
          durationMs = queueDuration;
          progressMs = std::min<int64_t>(queueDuration, std::max<int64_t>(0, elapsed));
          break;
        }
        elapsed -= queueDuration;
        ++index;
        if (index >= static_cast<int>(queue.Size())) {
          itemSource = JsonObject{};
          durationMs = 0;
          progressMs = 0;
          break;
        }
      }
    } else {
      const int64_t expectedEndAt = static_cast<int64_t>(std::max(0.0, NumberValue(value, L"expectedEndAt")));
      if (durationMs > 0 && expectedEndAt > 0) {
        progressMs = durationMs - std::max<int64_t>(0, expectedEndAt + kPlaybackTransitionHoldMs - fetchedAt);
      }
      else if (playing && sampledAt > 0) progressMs += std::max<int64_t>(0, fetchedAt - sampledAt);
    }

    JsonObject item = NormalizeItem(dataDir, itemSource);
    if (durationMs <= 0) durationMs = static_cast<int64_t>(NumberValue(item, L"durationMs"));
    if (durationMs > 0) progressMs = std::clamp<int64_t>(progressMs, 0, durationMs);

    JsonObject resolved;
    const std::wstring title = StringValue(item, L"title");
    resolved.SetNamedValue(L"title", JsonValue::CreateStringValue(title));
    resolved.SetNamedValue(L"artist", JsonValue::CreateStringValue(StringValue(item, L"artist")));
    resolved.SetNamedValue(L"artwork", JsonValue::CreateStringValue(StringValue(item, L"artwork")));
    resolved.SetNamedValue(L"durationMs", JsonValue::CreateNumberValue(static_cast<double>(durationMs)));
    resolved.SetNamedValue(L"progressMs", JsonValue::CreateNumberValue(static_cast<double>(progressMs)));
    resolved.SetNamedValue(L"playing", JsonValue::CreateBooleanValue(playing));
    resolved.SetNamedValue(L"hasTrack", JsonValue::CreateBooleanValue(!title.empty()));
    resolved.SetNamedValue(L"sampledAt", JsonValue::CreateNumberValue(static_cast<double>(fetchedAt)));
    resolved.SetNamedValue(L"anchorAt", JsonValue::CreateNumberValue(
        static_cast<double>(playing ? fetchedAt - progressMs : 0)));
    resolved.SetNamedValue(L"queueEndAt", JsonValue::CreateNumberValue(static_cast<double>(queueEndAt)));
    return resolved.Stringify().c_str();
  } catch (...) {
    return L"{}";
  }
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

void SaveNativePlaybackSnapshot(const fs::path& dataDir, const wchar_t* source,
                                const std::wstring& payload,
                                const std::wstring& error,
                                int64_t fetchedAt) {
  if (!source || !*source) return;
  std::wostringstream json;
  json << L"{\"source\":" << JsonQuote(source)
       << L",\"fetchedAt\":" << fetchedAt
       << L",\"ok\":" << (error.empty() ? L"true" : L"false");
  if (!error.empty()) json << L",\"error\":" << JsonQuote(error);
  if (!payload.empty()) json << L",\"payload\":" << payload;
  json << L"}";
  AtomicWriteText(dataDir / (std::wstring(L"native-playback-") + source + L".json"),
                  WideToUtf8(json.str()));
}

struct NativePlaybackSnapshot {
  std::wstring source;
  std::wstring payload;
  std::wstring resolved;
  std::wstring error;
  int64_t fetchedAt = 0;
  bool hasPayload = false;
};

bool LoadNativePlaybackSnapshot(const fs::path& dataDir, const wchar_t* source,
                                NativePlaybackSnapshot* snapshot) {
  if (!source || !*source || !snapshot) return false;
  try {
    std::ifstream input(
        dataDir / (std::wstring(L"native-playback-") + source + L".json"),
        std::ios::binary);
    if (!input) return false;
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    if (text.empty()) return false;
    const JsonObject root = JsonObject::Parse(Utf8ToWide(text));
    const std::wstring payload = StringValue(root, L"payload");
    const std::wstring error = StringValue(root, L"error");
    const int64_t fetchedAt = static_cast<int64_t>(std::max(
        0.0, NumberValue(root, L"fetchedAt")));
    snapshot->source = source;
    snapshot->payload = payload;
    snapshot->error = error;
    snapshot->fetchedAt = fetchedAt;
    snapshot->resolved =
        payload.empty() ? L"{}" : ResolvePlaybackPayload(dataDir, payload, fetchedAt);
    snapshot->hasPayload = error.empty() && !payload.empty();
    return snapshot->hasPayload || !error.empty();
  } catch (...) {
    return false;
  }
}
}  // namespace

void Renderer::StartNativePlaybackBridge() {
  if (nativePlaybackStarted_.exchange(true, std::memory_order_acq_rel)) return;
  nativePlaybackStopping_ = false;
  {
    std::lock_guard lock(nativePlaybackMutex_);
    for (size_t index = 0; index < std::size(kPlaybackEndpoints); ++index) {
      NativePlaybackSnapshot loaded;
      if (!LoadNativePlaybackSnapshot(
              dataDir_, kPlaybackEndpoints[index].source, &loaded)) {
        continue;
      }
      auto& update = nativePlaybackUpdates_[index];
      update.source = std::move(loaded.source);
      update.payload = std::move(loaded.payload);
      update.error = std::move(loaded.error);
      update.fetchedAt = loaded.fetchedAt;
      update.resolved = std::move(loaded.resolved);
      update.hasPayload = loaded.hasPayload;
      update.revision = ++nativePlaybackRevision_;
    }
  }
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
      bool changed = false;
      {
        std::lock_guard lock(nativePlaybackMutex_);
        auto& update = nativePlaybackUpdates_[index];
        changed = update.payload != payload || update.error != error;
        if (!changed) continue;
        update.source = kPlaybackEndpoints[index].source;
        update.payload = std::move(payload);
        update.error = error;
        update.fetchedAt = UnixMillis();
        update.resolved = update.payload.empty() ? L"{}" : ResolvePlaybackPayload(dataDir_, update.payload, update.fetchedAt);
        update.hasPayload = update.error.empty() && !update.payload.empty();
        update.revision = ++nativePlaybackRevision_;
        SaveNativePlaybackSnapshot(
            dataDir_, update.source.c_str(), update.payload, update.error,
            update.fetchedAt);
      }
      if (changed) InvalidateRect(window_, nullptr, FALSE);
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
    if (!update.resolved.empty()) message << L",\"resolved\":" << update.resolved;
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
