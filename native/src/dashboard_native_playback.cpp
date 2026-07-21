#include "web_renderer.h"
#include "artwork_cache.h"
#include "json_helpers.h"
#include "winhttp_helpers.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {
constexpr wchar_t kDashboardUrl[] = L"https://skrzk.pages.dev/api/dashboard?history=0";
constexpr int64_t kDashboardPollIntervalMs = 5 * 60'000;
constexpr size_t kMaxDashboardResponseBytes = 4 * 1024 * 1024;
constexpr wchar_t kPlaybackSource[] = L"a";

using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;
using winrt::Windows::Data::Json::JsonValueType;

int64_t EpochMilliseconds(double value) {
  if (!std::isfinite(value) || value <= 0) return 0;
  if (value < 100'000'000'000.0) value *= 1000.0;
  return static_cast<int64_t>(value);
}

int Integer(const JsonObject& object, const wchar_t* name, int fallback = 0) {
  const double value = json::Number(
      object, name, std::numeric_limits<double>::quiet_NaN());
  if (!std::isfinite(value) ||
      value < static_cast<double>(std::numeric_limits<int>::min()) ||
      value > static_cast<double>(std::numeric_limits<int>::max())) {
    return fallback;
  }
  return static_cast<int>(std::trunc(value));
}

NativePlaybackTrack ParseDashboardTrack(const fs::path& dataDir,
                                        const JsonObject& item) {
  NativePlaybackTrack track;
  track.title = json::Text(item, L"title");
  track.artist = json::Text(item, L"artist");
  track.artwork = CacheArtworkUrl(dataDir, json::Text(item, L"thumbnail_url"));
  track.durationMs = static_cast<int64_t>(std::max(
      0.0, json::Number(item, L"duration_ms")));
  return track;
}

NativePlaybackProjection ParseDashboardProjection(const fs::path& dataDir,
                                                   const std::wstring& payload,
                                                   int64_t fetchedAt) {
  NativePlaybackProjection projection;
  projection.fetchedAt = fetchedAt;
  if (payload.empty()) return projection;

  try {
    const JsonObject root = JsonObject::Parse(payload);
    if (!json::Boolean(root, L"ok")) return projection;

    const JsonArray queue = json::Array(root, L"queue");
    const JsonObject status = json::Object(root, L"queue_status");
    projection.queue.reserve(queue.Size());

    int markedCurrentIndex = -1;
    for (uint32_t index = 0; index < queue.Size(); ++index) {
      if (queue.GetAt(index).ValueType() != JsonValueType::Object) {
        projection.queue.emplace_back();
        continue;
      }
      const JsonObject item = queue.GetAt(index).GetObject();
      if (markedCurrentIndex < 0 && json::Boolean(item, L"is_current")) {
        markedCurrentIndex = static_cast<int>(index);
      }
      projection.queue.push_back(ParseDashboardTrack(dataDir, item));
    }

    int currentIndex = Integer(status, L"current_index", markedCurrentIndex);
    if (currentIndex < 0 ||
        currentIndex >= static_cast<int>(projection.queue.size())) {
      currentIndex = markedCurrentIndex;
    }
    if (currentIndex < 0 ||
        currentIndex >= static_cast<int>(projection.queue.size())) {
      currentIndex = -1;
    }

    const bool paused = json::Boolean(status, L"is_paused");
    const bool playing = json::Boolean(status, L"playing") && !paused &&
        currentIndex >= 0;
    const int64_t generatedAt = EpochMilliseconds(
        json::Number(root, L"generated_at"));
    const int64_t anchorAt = EpochMilliseconds(
        json::Number(status, L"anchor_at"));
    const int64_t queueEndAt = EpochMilliseconds(
        json::Number(status, L"queue_end_at"));

    int64_t progressMs = static_cast<int64_t>(std::max(
        0.0, json::Number(status, L"progress_ms")));
    if (currentIndex >= 0 && currentIndex < static_cast<int>(queue.Size())) {
      try {
        const JsonObject current = queue.GetAt(currentIndex).GetObject();
        progressMs = static_cast<int64_t>(std::max(
            static_cast<double>(progressMs),
            json::Number(current, L"progress_ms")));
      } catch (...) {
      }
    }

    if (playing) {
      if (anchorAt > 0 && generatedAt > 0) {
        progressMs = std::max<int64_t>(0, generatedAt - anchorAt) +
            std::max<int64_t>(0, fetchedAt - generatedAt);
      } else if (generatedAt > 0) {
        progressMs += std::max<int64_t>(0, fetchedAt - generatedAt);
      }
    }

    if (currentIndex >= 0) {
      const int64_t durationMs =
          projection.queue[static_cast<size_t>(currentIndex)].durationMs;
      if (durationMs > 0) progressMs = std::min(progressMs, durationMs);
    }

    projection.available = true;
    projection.playing = playing;
    projection.currentIndex = currentIndex;
    projection.progressMs = std::max<int64_t>(0, progressMs);
    projection.sampledAt = fetchedAt;
    projection.anchorAt = playing ? fetchedAt - projection.progressMs : 0;
    projection.queueRevision = json::Text(root, L"queue_revision");
    if (queueEndAt > 0 && generatedAt > 0) {
      projection.queueEndAt = fetchedAt +
          std::max<int64_t>(0, queueEndAt - generatedAt);
    } else {
      projection.queueEndAt = queueEndAt;
    }
    return projection;
  } catch (...) {
    NativePlaybackProjection failed;
    failed.fetchedAt = fetchedAt;
    return failed;
  }
}

std::wstring FetchDashboardJson(std::wstring* payload) {
  if (!payload) return L"dashboard payload missing";
  payload->clear();

  std::wstring requestUrl(kDashboardUrl);
  requestUrl += L"&_hp=" + std::to_wstring(UnixMillis());

  std::vector<uint8_t> body;
  std::wstring error;
  if (!WinHttpDownload(
          requestUrl, kMaxDashboardResponseBytes, &body, nullptr, &error,
          L"HomePanel-Native-Dashboard/1.0",
          L"Accept: application/json\r\nCache-Control: no-cache, no-store\r\nPragma: no-cache\r\n")) {
    return error.empty() ? L"dashboard download failed" : error;
  }

  const std::wstring wide = Utf8ToWide(std::string(body.begin(), body.end()));
  if (wide.empty()) return L"invalid UTF-8 dashboard response";
  try {
    const JsonValue parsed = JsonValue::Parse(wide);
    if (parsed.ValueType() != JsonValueType::Object) {
      return L"dashboard response is not a JSON object";
    }
    const JsonObject root = parsed.GetObject();
    if (!json::Boolean(root, L"ok")) {
      return json::Text(root, L"error", L"dashboard API returned ok=false");
    }
  } catch (...) {
    return L"invalid dashboard JSON";
  }

  *payload = wide;
  return {};
}

fs::path SnapshotPath(const fs::path& dataDir) {
  return dataDir / L"native-playback-a.json";
}

void SaveDashboardSnapshot(const fs::path& dataDir,
                           const std::wstring& payload,
                           const std::wstring& error,
                           int64_t fetchedAt) {
  std::wostringstream output;
  output << L"{\"source\":\"a\",\"fetchedAt\":" << fetchedAt
         << L",\"ok\":" << (error.empty() ? L"true" : L"false");
  if (!error.empty()) output << L",\"error\":" << JsonQuote(error);
  if (!payload.empty()) output << L",\"payload\":" << payload;
  output << L"}";
  AtomicWriteText(SnapshotPath(dataDir), WideToUtf8(output.str()));
}

bool LoadDashboardSnapshot(const fs::path& dataDir,
                           std::wstring* payload,
                           std::wstring* error,
                           int64_t* fetchedAt) {
  if (!payload || !error || !fetchedAt) return false;
  try {
    std::ifstream input(SnapshotPath(dataDir), std::ios::binary);
    if (!input) return false;
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    if (text.empty()) return false;
    const JsonObject root = JsonObject::Parse(Utf8ToWide(text));
    *payload = json::Stringify(root, L"payload");
    *error = json::Text(root, L"error");
    *fetchedAt = static_cast<int64_t>(std::max(
        0.0, json::Number(root, L"fetchedAt")));
    return !payload->empty() || !error->empty();
  } catch (...) {
    return false;
  }
}
}  // namespace

void Renderer::StartNativePlaybackBridge() {
  if (nativePlaybackStarted_.exchange(true, std::memory_order_acq_rel)) return;
  nativePlaybackStopping_ = false;

  std::wstring payload;
  std::wstring error;
  int64_t fetchedAt = 0;
  if (LoadDashboardSnapshot(dataDir_, &payload, &error, &fetchedAt)) {
    std::lock_guard lock(nativePlaybackMutex_);
    NativePlaybackUpdate& update = nativePlaybackUpdates_[0];
    update.source = kPlaybackSource;
    update.payload = payload;
    update.error = error;
    update.fetchedAt = fetchedAt;
    update.projection = ParseDashboardProjection(dataDir_, payload, fetchedAt);
    update.hasPayload = error.empty() && !payload.empty();
    update.contentRevision = ++nativePlaybackContentRevision_;
    update.revision = ++nativePlaybackRevision_;
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
  const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  while (!nativePlaybackStopping_.load(std::memory_order_acquire)) {
    std::wstring payload;
    const int64_t fetchedAt = UnixMillis();
    const std::wstring error = FetchDashboardJson(&payload);
    NativePlaybackProjection projection =
        ParseDashboardProjection(dataDir_, payload, fetchedAt);
    SaveDashboardSnapshot(dataDir_, payload, error, fetchedAt);

    {
      std::lock_guard lock(nativePlaybackMutex_);
      NativePlaybackUpdate& update = nativePlaybackUpdates_[0];
      update.source = kPlaybackSource;
      update.error = error;
      update.fetchedAt = fetchedAt;
      const bool hasValidPayload = error.empty() && !payload.empty();
      if (hasValidPayload) {
        const bool contentChanged = !update.hasPayload ||
            (!projection.queueRevision.empty()
                 ? projection.queueRevision != update.projection.queueRevision
                 : update.payload != payload);
        update.payload = std::move(payload);
        update.projection = std::move(projection);
        update.hasPayload = true;
        if (contentChanged) {
          update.contentRevision = ++nativePlaybackContentRevision_;
        }
      }
      update.revision = ++nativePlaybackRevision_;
    }

    InvalidatePanelSection(nativeMainWindow_, PanelSection::Music);
    std::unique_lock waitLock(nativePlaybackWakeMutex_);
    nativePlaybackWake_.wait_for(
        waitLock, std::chrono::milliseconds(kDashboardPollIntervalMs),
        [this] {
          return nativePlaybackStopping_.load(std::memory_order_acquire);
        });
  }
  if (SUCCEEDED(apartment)) CoUninitialize();
}

}  // namespace hp
