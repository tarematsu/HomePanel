#include "web_renderer.h"
#include "artwork_cache.h"
#include "json_helpers.h"
#include "winhttp_helpers.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {
constexpr wchar_t kDashboardUrl[] = L"https://skrzk.pages.dev/api/dashboard?history=0";
constexpr int64_t kDashboardPollIntervalMs = 5 * 60'000;
constexpr int64_t kDashboardSnapshotCheckpointMs = 30 * 60'000;
constexpr size_t kMaxDashboardResponseBytes = 4 * 1024 * 1024;
constexpr int kCompactSnapshotVersion = 2;
constexpr uint64_t kPayloadFnvOffset = 14695981039346656037ull;
constexpr uint64_t kPayloadFnvPrime = 1099511628211ull;

using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValueType;

void AppendSignatureBytes(uint64_t& hash, const void* value, size_t size) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(value);
  for (size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= kPayloadFnvPrime;
  }
}

template <typename T>
void AppendSignatureValue(uint64_t& hash, const T& value) noexcept {
  AppendSignatureBytes(hash, &value, sizeof(value));
}

void AppendSignatureText(uint64_t& hash, const std::wstring& value) noexcept {
  AppendSignatureBytes(hash, value.data(), value.size() * sizeof(wchar_t));
  const wchar_t separator = L'\0';
  AppendSignatureValue(hash, separator);
}

void AppendQueueSignature(uint64_t& hash,
                          const NativePlaybackProjection& projection) noexcept {
  AppendSignatureText(hash, projection.queueRevision);
  const size_t queueSize = projection.queue.size();
  AppendSignatureValue(hash, queueSize);
  for (const NativePlaybackTrack& track : projection.queue) {
    AppendSignatureText(hash, track.title);
    AppendSignatureText(hash, track.artist);
    AppendSignatureText(hash, track.artwork);
    AppendSignatureValue(hash, track.durationMs);
  }
}

uint64_t PlaybackSnapshotSignature(
    const NativePlaybackProjection& projection) noexcept {
  uint64_t hash = kPayloadFnvOffset;
  AppendQueueSignature(hash, projection);
  AppendSignatureValue(hash, projection.currentIndex);
  AppendSignatureValue(hash, projection.available);
  AppendSignatureValue(hash, projection.ended);
  AppendSignatureValue(hash, projection.setupRequired);
  return hash;
}

uint64_t PlaybackPersistenceSignature(
    const NativePlaybackProjection& projection) noexcept {
  uint64_t hash = kPayloadFnvOffset;
  AppendQueueSignature(hash, projection);
  AppendSignatureValue(hash, projection.available);
  AppendSignatureValue(hash, projection.ended);
  AppendSignatureValue(hash, projection.setupRequired);
  return hash;
}

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

int64_t Integer64(const JsonObject& object, const wchar_t* name,
                  int64_t fallback = 0) {
  const double value = json::Number(
      object, name, std::numeric_limits<double>::quiet_NaN());
  if (!std::isfinite(value) ||
      value < static_cast<double>(std::numeric_limits<int64_t>::min()) ||
      value > static_cast<double>(std::numeric_limits<int64_t>::max())) {
    return fallback;
  }
  return static_cast<int64_t>(std::trunc(value));
}

bool BooleanOrNumber(const JsonObject& object, const wchar_t* name,
                     bool fallback = false) {
  try {
    const auto value = object.GetNamedValue(name);
    if (value.ValueType() == JsonValueType::Boolean) return value.GetBoolean();
    if (value.ValueType() == JsonValueType::Number) return value.GetNumber() != 0;
  } catch (...) {
  }
  return fallback;
}

bool ProjectionAtQueueEnd(const NativePlaybackProjection& projection,
                          int64_t nowMs) {
  if (!projection.available || projection.setupRequired) return false;
  if (projection.ended) return true;
  return !projection.queue.empty() && projection.queueEndAt > 0 &&
      nowMs >= projection.queueEndAt;
}

bool ProjectionHasPlayableTrack(const NativePlaybackProjection& projection,
                                int64_t nowMs) {
  if (ProjectionAtQueueEnd(projection, nowMs) || projection.currentIndex < 0 ||
      projection.currentIndex >= static_cast<int>(projection.queue.size())) {
    return false;
  }
  const NativePlaybackTrack& track =
      projection.queue[static_cast<size_t>(projection.currentIndex)];
  return !track.title.empty() || !track.artist.empty() || !track.artwork.empty();
}

NativeMinuteFactsProjection ParseDashboardStatus(const JsonObject& root,
                                                  int64_t fetchedAt) {
  NativeMinuteFactsProjection projection;
  projection.fetchedAt = fetchedAt;
  try {
    projection.ok = BooleanOrNumber(root, L"ok");
    if (!projection.ok) return projection;

    const JsonObject latest = json::Object(root, L"latest");
    if (latest.Size() == 0) return projection;
    const JsonObject queueStatus = json::Object(root, L"queue_status");

    projection.stale = BooleanOrNumber(root, L"stale");
    projection.isBroadcasting = BooleanOrNumber(latest, L"is_broadcasting");
    projection.isPaused = BooleanOrNumber(queueStatus, L"is_paused");
    projection.listenerCount = std::max(0, Integer(latest, L"listener_count"));
    projection.onlineMemberCount =
        std::max(0, Integer(latest, L"online_member_count"));
    projection.minuteAt = EpochMilliseconds(json::Number(root, L"generated_at"));
    projection.available = true;
    return projection;
  } catch (...) {
    NativeMinuteFactsProjection failed;
    failed.fetchedAt = fetchedAt;
    return failed;
  }
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

bool ParseDashboardPayload(const fs::path& dataDir,
                           const std::wstring& payload,
                           int64_t fetchedAt,
                           NativePlaybackProjection* playbackProjection,
                           NativeMinuteFactsProjection* statusProjection,
                           std::wstring* error) {
  if (!playbackProjection || !statusProjection || payload.empty()) return false;
  NativePlaybackProjection projection;
  projection.fetchedAt = fetchedAt;
  NativeMinuteFactsProjection facts;
  facts.fetchedAt = fetchedAt;

  try {
    const JsonObject root = JsonObject::Parse(payload);
    if (!json::Boolean(root, L"ok")) {
      if (error) {
        *error = json::Text(root, L"error", L"dashboard API returned ok=false");
      }
      return false;
    }
    facts = ParseDashboardStatus(root, fetchedAt);

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
      if (markedCurrentIndex < 0 && BooleanOrNumber(item, L"is_current")) {
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

    const bool paused = BooleanOrNumber(status, L"is_paused");
    const bool playingSignal = BooleanOrNumber(
        status, L"playing", BooleanOrNumber(root, L"playing"));
    const bool playing = playingSignal && !paused && currentIndex >= 0;
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
    projection.stale = BooleanOrNumber(root, L"stale");
    projection.ended = BooleanOrNumber(
        status, L"ended", BooleanOrNumber(root, L"ended"));
    projection.setupRequired = BooleanOrNumber(root, L"setup_required");
    projection.currentIndex = currentIndex;
    projection.progressMs = std::max<int64_t>(0, progressMs);
    projection.sampledAt = fetchedAt;
    projection.anchorAt = playing ? fetchedAt - projection.progressMs : 0;
    projection.queueRevision = json::Text(root, L"queue_revision");
    projection.queueEndAt = queueEndAt > 0 && generatedAt > 0
        ? fetchedAt + std::max<int64_t>(0, queueEndAt - generatedAt)
        : queueEndAt;

    *playbackProjection = std::move(projection);
    *statusProjection = facts;
    return true;
  } catch (...) {
    if (error) *error = L"invalid dashboard JSON";
    return false;
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

  const std::string utf8(body.begin(), body.end());
  std::wstring wide = Utf8ToWide(utf8);
  if (wide.empty()) return L"invalid UTF-8 dashboard response";
  *payload = std::move(wide);
  return {};
}

fs::path SnapshotPath(const fs::path& dataDir) {
  return dataDir / L"native-playback-a.json";
}

void AppendJsonBoolean(std::wostringstream& output, bool value) {
  output << (value ? L"true" : L"false");
}

bool SaveDashboardSnapshot(const fs::path& dataDir,
                           const NativePlaybackProjection& playback,
                           const NativeMinuteFactsProjection& facts,
                           int64_t fetchedAt) {
  std::wostringstream output;
  output << L"{\"version\":" << kCompactSnapshotVersion
         << L",\"source\":\"a\",\"fetchedAt\":" << fetchedAt
         << L",\"playback\":{";
  output << L"\"available\":"; AppendJsonBoolean(output, playback.available);
  output << L",\"playing\":"; AppendJsonBoolean(output, playback.playing);
  output << L",\"stale\":"; AppendJsonBoolean(output, playback.stale);
  output << L",\"ended\":"; AppendJsonBoolean(output, playback.ended);
  output << L",\"setupRequired\":"; AppendJsonBoolean(output, playback.setupRequired);
  output << L",\"queueRevision\":" << JsonQuote(playback.queueRevision)
         << L",\"currentIndex\":" << playback.currentIndex
         << L",\"progressMs\":" << playback.progressMs
         << L",\"anchorAt\":" << playback.anchorAt
         << L",\"sampledAt\":" << playback.sampledAt
         << L",\"queueEndAt\":" << playback.queueEndAt
         << L",\"queue\":[";
  for (size_t index = 0; index < playback.queue.size(); ++index) {
    if (index) output << L',';
    const NativePlaybackTrack& track = playback.queue[index];
    output << L"{\"title\":" << JsonQuote(track.title)
           << L",\"artist\":" << JsonQuote(track.artist)
           << L",\"artwork\":" << JsonQuote(track.artwork)
           << L",\"durationMs\":" << track.durationMs << L'}';
  }
  output << L"]},\"facts\":{";
  output << L"\"available\":"; AppendJsonBoolean(output, facts.available);
  output << L",\"ok\":"; AppendJsonBoolean(output, facts.ok);
  output << L",\"stale\":"; AppendJsonBoolean(output, facts.stale);
  output << L",\"isBroadcasting\":"; AppendJsonBoolean(output, facts.isBroadcasting);
  output << L",\"isPaused\":"; AppendJsonBoolean(output, facts.isPaused);
  output << L",\"listenerCount\":" << facts.listenerCount
         << L",\"onlineMemberCount\":" << facts.onlineMemberCount
         << L",\"minuteAt\":" << facts.minuteAt
         << L",\"fetchedAt\":" << facts.fetchedAt << L"}}";
  return AtomicWriteText(SnapshotPath(dataDir), WideToUtf8(output.str()));
}

bool LoadCompactSnapshot(const JsonObject& root,
                         NativePlaybackProjection* playback,
                         NativeMinuteFactsProjection* facts) {
  if (!playback || !facts || Integer(root, L"version") < kCompactSnapshotVersion) {
    return false;
  }
  const int64_t rootFetchedAt = Integer64(root, L"fetchedAt");
  const JsonObject savedPlayback = json::Object(root, L"playback");
  if (savedPlayback.Size() == 0) return false;

  NativePlaybackProjection projection;
  projection.available = BooleanOrNumber(savedPlayback, L"available");
  projection.playing = BooleanOrNumber(savedPlayback, L"playing");
  projection.stale = BooleanOrNumber(savedPlayback, L"stale");
  projection.ended = BooleanOrNumber(savedPlayback, L"ended");
  projection.setupRequired = BooleanOrNumber(savedPlayback, L"setupRequired");
  projection.queueRevision = json::Text(savedPlayback, L"queueRevision");
  projection.currentIndex = Integer(savedPlayback, L"currentIndex", -1);
  projection.progressMs = std::max<int64_t>(0, Integer64(savedPlayback, L"progressMs"));
  projection.anchorAt = Integer64(savedPlayback, L"anchorAt");
  projection.sampledAt = Integer64(savedPlayback, L"sampledAt", rootFetchedAt);
  projection.queueEndAt = Integer64(savedPlayback, L"queueEndAt");
  projection.fetchedAt = rootFetchedAt;
  const JsonArray queue = json::Array(savedPlayback, L"queue");
  projection.queue.reserve(queue.Size());
  for (uint32_t index = 0; index < queue.Size(); ++index) {
    if (queue.GetAt(index).ValueType() != JsonValueType::Object) {
      projection.queue.emplace_back();
      continue;
    }
    const JsonObject item = queue.GetAt(index).GetObject();
    projection.queue.push_back(NativePlaybackTrack{
        json::Text(item, L"title"),
        json::Text(item, L"artist"),
        json::Text(item, L"artwork"),
        std::max<int64_t>(0, Integer64(item, L"durationMs")),
    });
  }
  if (projection.currentIndex < -1 ||
      projection.currentIndex >= static_cast<int>(projection.queue.size())) {
    projection.currentIndex = -1;
  }

  NativeMinuteFactsProjection savedFacts;
  const JsonObject factsObject = json::Object(root, L"facts");
  if (factsObject.Size() > 0) {
    savedFacts.available = BooleanOrNumber(factsObject, L"available");
    savedFacts.ok = BooleanOrNumber(factsObject, L"ok");
    savedFacts.stale = BooleanOrNumber(factsObject, L"stale");
    savedFacts.isBroadcasting = BooleanOrNumber(factsObject, L"isBroadcasting");
    savedFacts.isPaused = BooleanOrNumber(factsObject, L"isPaused");
    savedFacts.listenerCount = std::max(0, Integer(factsObject, L"listenerCount"));
    savedFacts.onlineMemberCount =
        std::max(0, Integer(factsObject, L"onlineMemberCount"));
    savedFacts.minuteAt = Integer64(factsObject, L"minuteAt");
    savedFacts.fetchedAt = Integer64(factsObject, L"fetchedAt", rootFetchedAt);
  }

  *playback = std::move(projection);
  *facts = savedFacts;
  return playback->available;
}

bool LoadDashboardSnapshot(const fs::path& dataDir,
                           NativePlaybackProjection* playback,
                           NativeMinuteFactsProjection* facts) {
  if (!playback || !facts) return false;
  try {
    std::ifstream input(SnapshotPath(dataDir), std::ios::binary);
    if (!input) return false;
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    if (text.empty()) return false;
    const JsonObject root = JsonObject::Parse(Utf8ToWide(text));
    if (LoadCompactSnapshot(root, playback, facts)) return true;

    // Read snapshots created by pre-v2 builds once, then replace them with the
    // compact projection format on the next successful network refresh.
    const std::wstring payload = json::Stringify(root, L"payload");
    const int64_t fetchedAt = Integer64(root, L"fetchedAt");
    std::wstring error;
    return !payload.empty() && ParseDashboardPayload(
        dataDir, payload, fetchedAt, playback, facts, &error);
  } catch (...) {
    return false;
  }
}
}  // namespace

void Renderer::StartNativePlaybackBridge() {
  if (nativePlaybackStarted_.exchange(true, std::memory_order_acq_rel)) return;
  nativePlaybackStopping_ = false;

  NativePlaybackProjection playbackProjection;
  NativeMinuteFactsProjection statusProjection;
  if (LoadDashboardSnapshot(dataDir_, &playbackProjection, &statusProjection)) {
    const uint64_t signature = PlaybackSnapshotSignature(playbackProjection);
    {
      std::lock_guard lock(nativePlaybackMutex_);
      NativePlaybackUpdate& update = nativePlaybackUpdate_;
      update.payloadSignature = signature;
      update.projection = std::move(playbackProjection);
      update.hasPayload = true;
      update.contentRevision = ++nativePlaybackContentRevision_;
    }
    {
      std::lock_guard lock(nativeMinuteFactsMutex_);
      nativeMinuteFacts_ = statusProjection;
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
  const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  int64_t lastSnapshotSavedAt = 0;
  uint64_t lastPersistenceSignature = 0;
  {
    std::lock_guard lock(nativePlaybackMutex_);
    if (nativePlaybackUpdate_.hasPayload) {
      lastSnapshotSavedAt = nativePlaybackUpdate_.projection.fetchedAt;
      lastPersistenceSignature =
          PlaybackPersistenceSignature(nativePlaybackUpdate_.projection);
    }
  }

  while (!nativePlaybackStopping_.load(std::memory_order_acquire)) {
    std::wstring payload;
    std::wstring error = FetchDashboardJson(&payload);
    const int64_t fetchedAt = error.empty() ? UnixMillis() : 0;
    NativePlaybackProjection projection;
    NativeMinuteFactsProjection statusProjection;
    bool hasValidPayload = error.empty() && ParseDashboardPayload(
        dataDir_, payload, fetchedAt, &projection, &statusProjection, &error);
    uint64_t projectionSignature = 0;
    if (hasValidPayload) {
      projectionSignature = PlaybackSnapshotSignature(projection);
      const uint64_t persistenceSignature =
          PlaybackPersistenceSignature(projection);
      const bool snapshotChanged =
          persistenceSignature != lastPersistenceSignature;
      const bool checkpointDue = lastSnapshotSavedAt <= 0 ||
          fetchedAt - lastSnapshotSavedAt >= kDashboardSnapshotCheckpointMs;
      if ((snapshotChanged || checkpointDue) &&
          SaveDashboardSnapshot(dataDir_, projection, statusProjection, fetchedAt)) {
        lastSnapshotSavedAt = fetchedAt;
        lastPersistenceSignature = persistenceSignature;
      }
    }

    bool musicChanged = false;
    {
      std::lock_guard lock(nativePlaybackMutex_);
      NativePlaybackUpdate& update = nativePlaybackUpdate_;
      if (hasValidPayload) {
        const bool previousPlayable =
            ProjectionHasPlayableTrack(update.projection, fetchedAt);
        const bool nextPlayable = ProjectionHasPlayableTrack(projection, fetchedAt);
        const bool queueChanged =
            update.payloadSignature != projectionSignature;
        musicChanged = !update.hasPayload || queueChanged ||
            update.projection.available != projection.available ||
            update.projection.playing != projection.playing ||
            update.projection.stale != projection.stale ||
            update.projection.ended != projection.ended ||
            update.projection.setupRequired != projection.setupRequired ||
            update.projection.currentIndex != projection.currentIndex;
        const bool advanceContentRevision = !update.hasPayload ||
            (nextPlayable && (!previousPlayable || queueChanged));
        update.payloadSignature = projectionSignature;
        update.projection = std::move(projection);
        update.hasPayload = true;
        // Keep the fallback release revision stable while responses still have
        // no usable current track. Advance it only when playable queue data
        // first appears or changes to a new playable queue.
        if (advanceContentRevision) {
          update.contentRevision = ++nativePlaybackContentRevision_;
        }
      }
    }
    if (hasValidPayload) {
      std::lock_guard lock(nativeMinuteFactsMutex_);
      nativeMinuteFacts_ = statusProjection;
    }

    if (musicChanged) {
      InvalidatePanelSection(nativeMainWindow_, PanelSection::Music);
    }
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