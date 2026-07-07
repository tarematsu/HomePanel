#include "cloud_client.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {
constexpr wchar_t kPlaybackUrl[] =
    L"https://api.spotify.com/v1/me/player?additional_types=track,episode";
constexpr int64_t kTokenSkewMs = 60'000;
constexpr int64_t kIdlePollMs = 5'000;
constexpr int64_t kMinimumPlayingPollMs = 5'000;
constexpr int64_t kShortTrackThresholdMs = 3 * 60'000;
constexpr int64_t kTrackTransitionGraceMs = 2'000;

std::atomic<int64_t> gSpotifyNextPollAt{0};

struct SpotifyPollPlan {
  int64_t delayMs = kIdlePollMs;
  std::wstring reason = L"idle";
};

SpotifyPollPlan PlanSpotifyPoll(double durationValue, double progressValue) {
  const int64_t duration =
      std::max<int64_t>(0, static_cast<int64_t>(durationValue));
  if (duration <= 0) return {};

  const int64_t progress = std::clamp<int64_t>(
      static_cast<int64_t>(progressValue), 0, duration);
  const int64_t remaining = duration - progress;
  if (remaining <= 0) {
    return {kMinimumPlayingPollMs, L"transition-retry"};
  }

  if (duration > kShortTrackThresholdMs) {
    const int64_t midpoint = duration / 2;
    if (progress < midpoint) {
      return {
          std::max<int64_t>(kMinimumPlayingPollMs, midpoint - progress),
          L"midpoint"};
    }
  }

  return {
      std::max<int64_t>(
          kMinimumPlayingPollMs, remaining + kTrackTransitionGraceMs),
      L"next-track"};
}

int64_t ScheduleSpotifyPoll(int64_t sampledAt, int64_t delayMs) {
  const int64_t nextPollAt = sampledAt + std::max<int64_t>(1'000, delayMs);
  gSpotifyNextPollAt.store(nextPollAt, std::memory_order_release);
  return nextPollAt;
}

void AddSpotifyPollMetadata(winrt::Windows::Data::Json::JsonObject& output,
                            int64_t sampledAt, int64_t nextPollAt,
                            const std::wstring& reason) {
  output.SetNamedValue(
      L"pollIntervalMs",
      winrt::Windows::Data::Json::JsonValue::CreateNumberValue(
          static_cast<double>(std::max<int64_t>(0, nextPollAt - sampledAt))));
  output.SetNamedValue(
      L"nextPollAt",
      winrt::Windows::Data::Json::JsonValue::CreateNumberValue(
          static_cast<double>(nextPollAt)));
  output.SetNamedValue(
      L"nextPollReason",
      winrt::Windows::Data::Json::JsonValue::CreateStringValue(reason));
}

struct WebResult {
  DWORD status = 0;
  std::string body;
};

std::string WinHttpErrorText(const char* action) {
  return std::string(action) + " failed (" + std::to_string(GetLastError()) + ")";
}

WebResult WebRequest(const std::wstring& method, const std::wstring& url,
                     const std::wstring& bearer = {}) {
  URL_COMPONENTS parts{sizeof(parts)};
  wchar_t host[256]{};
  wchar_t path[4096]{};
  parts.lpszHostName = host;
  parts.dwHostNameLength = _countof(host);
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = _countof(path);
  parts.dwSchemeLength = 1;
  parts.dwExtraInfoLength = 1;
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
    throw std::runtime_error("invalid Spotify URL");
  }
  std::wstring requestPath(path, parts.dwUrlPathLength);
  if (parts.lpszExtraInfo && parts.dwExtraInfoLength) {
    requestPath.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  }

  for (DWORD accessType : {WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_ACCESS_TYPE_NO_PROXY}) {
    HINTERNET session = WinHttpOpen(
        L"HomePanel-Spotify/2.0", accessType,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
      if (accessType == WINHTTP_ACCESS_TYPE_NO_PROXY) {
        throw std::runtime_error(WinHttpErrorText("Spotify WinHttpOpen"));
      }
      continue;
    }
    HINTERNET connection = WinHttpConnect(
        session, std::wstring(host, parts.dwHostNameLength).c_str(), parts.nPort, 0);
    if (!connection) {
      WinHttpCloseHandle(session);
      if (accessType == WINHTTP_ACCESS_TYPE_NO_PROXY) {
        throw std::runtime_error(WinHttpErrorText("Spotify WinHttpConnect"));
      }
      continue;
    }
    HINTERNET request = WinHttpOpenRequest(
        connection, method.c_str(), requestPath.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
      WinHttpCloseHandle(connection);
      WinHttpCloseHandle(session);
      if (accessType == WINHTTP_ACCESS_TYPE_NO_PROXY) {
        throw std::runtime_error(WinHttpErrorText("Spotify WinHttpOpenRequest"));
      }
      continue;
    }
    WinHttpSetTimeouts(request, 8000, 8000, 8000, 8000);
    std::wstring headers = L"Accept: application/json\r\n";
    if (!bearer.empty()) {
      headers += L"Authorization: Bearer " + bearer + L"\r\n";
    }
    const BOOL sent = WinHttpSendRequest(
        request, headers.c_str(), static_cast<DWORD>(headers.size()),
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
      const std::string detail = "Spotify request failed (" + std::to_string(GetLastError()) + ")";
      WinHttpCloseHandle(request);
      WinHttpCloseHandle(connection);
      WinHttpCloseHandle(session);
      if (accessType == WINHTTP_ACCESS_TYPE_NO_PROXY) throw std::runtime_error(detail);
      continue;
    }

    WebResult result;
    DWORD statusSize = sizeof(result.status);
    WinHttpQueryHeaders(
        request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &result.status, &statusSize,
        WINHTTP_NO_HEADER_INDEX);
    bool ok = true;
    std::string failure;
    for (;;) {
      DWORD available = 0;
      if (!WinHttpQueryDataAvailable(request, &available)) {
        ok = false;
        failure = WinHttpErrorText("Spotify response query");
        break;
      }
      if (!available) break;
      const size_t offset = result.body.size();
      result.body.resize(offset + available);
      DWORD read = 0;
      if (!WinHttpReadData(
              request, result.body.data() + offset, available, &read)) {
        ok = false;
        failure = WinHttpErrorText("Spotify response read");
        break;
      }
      result.body.resize(offset + read);
      if (!read) break;
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (ok) return result;
    if (accessType == WINHTTP_ACCESS_TYPE_NO_PROXY) throw std::runtime_error(failure);
  }
  throw std::runtime_error("Spotify request failed");
}

class SpotifyRuntime {
 public:
  SpotifyRuntime() : thread_([this] {
    while (!stop_) {
      const int64_t now = UnixMillis();
      const int64_t nextPollAt =
          gSpotifyNextPollAt.load(std::memory_order_acquire);
      if (nextPollAt <= 0 || now >= nextPollAt) {
        gSpotifyNextPollAt.store(now + kIdlePollMs,
                                 std::memory_order_release);
        CloudClient::RefreshCurrentSpotifySafely();
      }
      for (int i = 0; i < 10 && !stop_; ++i) Sleep(100);
    }
  }) {}

  ~SpotifyRuntime() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }

 private:
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

SpotifyRuntime gSpotifyRuntime;
}  // namespace

void CloudClient::RefreshCurrentSpotifySafely() noexcept {
  std::lock_guard runtimeCall(spotifyRuntimeCallMutex_);
  CloudClient* client = current_.load(std::memory_order_acquire);
  if (client) client->RefreshSpotifyNow();
}

std::wstring CloudClient::BeginSpotifyAuthorization() {
  try {
    if (deviceToken_.empty() || config_.cloudflareBaseUrl.empty()) {
      WriteSpotifyUnavailable(L"Cloud Spotify authorization is not configured");
      return {};
    }
    const HttpResponse response = Request(
        L"POST", L"/v1/spotify/authorize", deviceToken_, {}, "{}",
        L"application/json");
    if (response.status != 200) {
      throw std::runtime_error(
          "Spotify authorization HTTP " + std::to_string(response.status));
    }
    const std::string body(response.body.begin(), response.body.end());
    const auto root = winrt::Windows::Data::Json::JsonObject::Parse(
        Utf8ToWide(body));
    const std::wstring url =
        root.GetNamedString(L"authorizationUrl", L"").c_str();
    if (url.empty()) throw std::runtime_error("authorization URL missing");
    log_.Info(L"Cloud Spotify authorization URL received");
    return url;
  } catch (const std::exception& error) {
    const std::wstring detail = Utf8ToWide(error.what());
    log_.Warn(L"Cloud Spotify authorization setup failed: " + detail);
    WriteSpotifyUnavailable(detail);
    return {};
  }
}

bool CloudClient::IsSpotifyAuthorizationRedirect(const std::wstring&) const {
  return false;
}

bool CloudClient::CompleteSpotifyAuthorizationRedirect(const std::wstring&) {
  return false;
}

bool CloudClient::LoadSpotifyTokens() {
  const std::wstring text = LoadProtectedToken(
      dataDir_ / L"spotify-token.dat", L"HOMEPANEL_SPOTIFY_TOKEN_JSON");
  if (text.empty()) return false;
  try {
    const auto root = winrt::Windows::Data::Json::JsonObject::Parse(text);
    spotifyAccessToken_ =
        root.GetNamedString(L"accessToken", L"").c_str();
    spotifyExpiresAt_ =
        static_cast<int64_t>(root.GetNamedNumber(L"expiresAt", 0));
    spotifyRefreshToken_.clear();
    return !spotifyAccessToken_.empty() && spotifyExpiresAt_ > UnixMillis();
  } catch (...) {
    spotifyAccessToken_.clear();
    spotifyRefreshToken_.clear();
    spotifyExpiresAt_ = 0;
    return false;
  }
}

bool CloudClient::SaveSpotifyTokens() {
  winrt::Windows::Data::Json::JsonObject root;
  root.SetNamedValue(
      L"accessToken",
      winrt::Windows::Data::Json::JsonValue::CreateStringValue(
          spotifyAccessToken_));
  root.SetNamedValue(
      L"expiresAt",
      winrt::Windows::Data::Json::JsonValue::CreateNumberValue(
          static_cast<double>(spotifyExpiresAt_)));
  return SaveProtectedToken(
      dataDir_ / L"spotify-token.dat", root.Stringify().c_str());
}

bool CloudClient::ExchangeSpotifyCode(const std::wstring&) {
  return false;
}

bool CloudClient::RefreshSpotifyAccessToken() {
  if (deviceToken_.empty() || config_.cloudflareBaseUrl.empty()) return false;
  const HttpResponse response =
      Request(L"GET", L"/v1/spotify/access-token", deviceToken_);
  if (response.status != 200) return false;
  const std::string body(response.body.begin(), response.body.end());
  const auto root = winrt::Windows::Data::Json::JsonObject::Parse(
      Utf8ToWide(body));
  std::lock_guard lock(spotifyMutex_);
  spotifyAccessToken_ =
      root.GetNamedString(L"accessToken", L"").c_str();
  spotifyExpiresAt_ =
      static_cast<int64_t>(root.GetNamedNumber(L"expiresAt", 0));
  spotifyRefreshToken_.clear();
  return !spotifyAccessToken_.empty() && SaveSpotifyTokens();
}

void CloudClient::WriteSpotifyUnavailable(const std::wstring& error) {
  const bool configured =
      !config_.cloudflareBaseUrl.empty() && !deviceToken_.empty();
  const int64_t sampledAt = UnixMillis();
  const int64_t nextPollAt = ScheduleSpotifyPoll(sampledAt, kIdlePollMs);
  winrt::Windows::Data::Json::JsonObject root;
  root.SetNamedValue(
      L"configured",
      winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(configured));
  root.SetNamedValue(
      L"connected",
      winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(false));
  root.SetNamedValue(
      L"playing",
      winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(false));
  root.SetNamedValue(
      L"sampledAt",
      winrt::Windows::Data::Json::JsonValue::CreateNumberValue(
          static_cast<double>(sampledAt)));
  AddSpotifyPollMetadata(root, sampledAt, nextPollAt, L"retry");
  root.SetNamedValue(
      L"error",
      error.empty()
          ? winrt::Windows::Data::Json::JsonValue::CreateNullValue()
          : winrt::Windows::Data::Json::JsonValue::CreateStringValue(error));
  const std::string text = WideToUtf8(root.Stringify().c_str());
  if (AtomicWriteText(dataDir_ / L"spotify.json", text) && window_) {
    PostMessageW(window_, WM_HP_STATIONHEAD_CHANGED, 0, 0);
  }
}

void CloudClient::PollSpotify() {
  std::wstring access;
  const bool configured =
      !config_.cloudflareBaseUrl.empty() && !deviceToken_.empty();
  if (!configured) {
    WriteSpotifyUnavailable(L"Cloud Spotify authorization is not configured");
    return;
  }

  {
    std::lock_guard lock(spotifyMutex_);
    if (spotifyAccessToken_.empty()) LoadSpotifyTokens();
    if (!spotifyAccessToken_.empty() &&
        spotifyExpiresAt_ > UnixMillis() + kTokenSkewMs) {
      access = spotifyAccessToken_;
    }
  }
  if (access.empty()) {
    if (!RefreshSpotifyAccessToken()) {
      WriteSpotifyUnavailable(L"Spotify authorization required");
      return;
    }
    std::lock_guard lock(spotifyMutex_);
    access = spotifyAccessToken_;
  }

  WebResult response = WebRequest(L"GET", kPlaybackUrl, access);
  if (response.status == 401 && RefreshSpotifyAccessToken()) {
    {
      std::lock_guard lock(spotifyMutex_);
      access = spotifyAccessToken_;
    }
    response = WebRequest(L"GET", kPlaybackUrl, access);
  }

  const int64_t sampledAt = UnixMillis();
  int64_t pollDelayMs = kIdlePollMs;
  std::wstring pollReason = L"idle";
  winrt::Windows::Data::Json::JsonObject output;
  output.SetNamedValue(
      L"configured",
      winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(true));
  output.SetNamedValue(
      L"connected",
      winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(
          response.status == 200 || response.status == 204));
  output.SetNamedValue(
      L"sampledAt",
      winrt::Windows::Data::Json::JsonValue::CreateNumberValue(
          static_cast<double>(sampledAt)));

  if (response.status == 204) {
    output.SetNamedValue(
        L"playing",
        winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(false));
  } else if (response.status == 200) {
    const auto source = winrt::Windows::Data::Json::JsonObject::Parse(
        Utf8ToWide(response.body));
    const bool playing = source.GetNamedBoolean(L"is_playing", false);
    const double progress = source.GetNamedNumber(L"progress_ms", 0);
    output.SetNamedValue(
        L"playing",
        winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(playing));
    output.SetNamedValue(
        L"progressMs",
        winrt::Windows::Data::Json::JsonValue::CreateNumberValue(progress));

    if (source.HasKey(L"item") &&
        source.GetNamedValue(L"item").ValueType() ==
            winrt::Windows::Data::Json::JsonValueType::Object) {
      const auto item = source.GetNamedObject(L"item");
      const double duration = item.GetNamedNumber(L"duration_ms", 0);
      const double remaining = std::max(0.0, duration - progress);
      const int64_t expectedEndAt =
          playing && remaining > 0
              ? sampledAt + static_cast<int64_t>(remaining)
              : 0;
      output.SetNamedValue(
          L"durationMs",
          winrt::Windows::Data::Json::JsonValue::CreateNumberValue(duration));
      output.SetNamedValue(
          L"expectedEndAt",
          winrt::Windows::Data::Json::JsonValue::CreateNumberValue(
              static_cast<double>(expectedEndAt)));

      if (playing && duration > 0) {
        const SpotifyPollPlan plan = PlanSpotifyPoll(duration, progress);
        pollDelayMs = plan.delayMs;
        pollReason = plan.reason;
      }

      winrt::Windows::Data::Json::JsonObject normalized;
      normalized.SetNamedValue(
          L"name",
          winrt::Windows::Data::Json::JsonValue::CreateStringValue(
              item.GetNamedString(L"name", L"")));
      normalized.SetNamedValue(
          L"uri",
          winrt::Windows::Data::Json::JsonValue::CreateStringValue(
              item.GetNamedString(L"uri", L"")));
      std::wstring artist;
      if (item.HasKey(L"artists") &&
          item.GetNamedValue(L"artists").ValueType() ==
              winrt::Windows::Data::Json::JsonValueType::Array) {
        for (const auto value : item.GetNamedArray(L"artists")) {
          if (value.ValueType() !=
              winrt::Windows::Data::Json::JsonValueType::Object) continue;
          if (!artist.empty()) artist += L", ";
          artist += value.GetObject().GetNamedString(L"name", L"").c_str();
        }
      }
      normalized.SetNamedValue(
          L"artist",
          winrt::Windows::Data::Json::JsonValue::CreateStringValue(artist));
      normalized.SetNamedValue(
          L"durationMs",
          winrt::Windows::Data::Json::JsonValue::CreateNumberValue(duration));

      std::wstring artwork;
      if (item.HasKey(L"album") &&
          item.GetNamedValue(L"album").ValueType() ==
              winrt::Windows::Data::Json::JsonValueType::Object) {
        const auto album = item.GetNamedObject(L"album");
        if (album.HasKey(L"images") &&
            album.GetNamedValue(L"images").ValueType() ==
                winrt::Windows::Data::Json::JsonValueType::Array &&
            album.GetNamedArray(L"images").Size()) {
          artwork = album.GetNamedArray(L"images")
                        .GetObjectAt(0)
                        .GetNamedString(L"url", L"")
                        .c_str();
        }
      }
      normalized.SetNamedValue(
          L"artwork",
          winrt::Windows::Data::Json::JsonValue::CreateStringValue(artwork));
      output.SetNamedValue(L"item", normalized);
    }

    if (source.HasKey(L"device") &&
        source.GetNamedValue(L"device").ValueType() ==
            winrt::Windows::Data::Json::JsonValueType::Object) {
      const auto device = source.GetNamedObject(L"device");
      winrt::Windows::Data::Json::JsonObject normalized;
      normalized.SetNamedValue(
          L"name",
          winrt::Windows::Data::Json::JsonValue::CreateStringValue(
              device.GetNamedString(L"name", L"")));
      output.SetNamedValue(L"device", normalized);
    }
  } else {
    WriteSpotifyUnavailable(
        L"Spotify playback HTTP " + std::to_wstring(response.status));
    return;
  }

  const int64_t nextPollAt = ScheduleSpotifyPoll(sampledAt, pollDelayMs);
  AddSpotifyPollMetadata(output, sampledAt, nextPollAt, pollReason);
  const std::string text = WideToUtf8(output.Stringify().c_str());
  if (AtomicWriteText(dataDir_ / L"spotify.json", text) && window_) {
    PostMessageW(window_, WM_HP_STATIONHEAD_CHANGED, 0, 0);
  }
}

void CloudClient::RefreshSpotifyNow() {
  if (stopping_) return;
  std::lock_guard poll(spotifyPollMutex_);
  if (stopping_) return;
  try {
    PollSpotify();
  } catch (const std::exception& error) {
    const std::wstring detail = Utf8ToWide(error.what());
    log_.Warn(L"Local Spotify polling failed: " + detail);
    WriteSpotifyUnavailable(detail);
  }
}

void CloudClient::SpotifyLoop() {}

}  // namespace hp
