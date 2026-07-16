#include "web_renderer.h"
#include "artwork_cache.h"
#include "json_helpers.h"
#include "winhttp_helpers.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {
constexpr size_t kMaxPlaybackResponseBytes = 4 * 1024 * 1024;
constexpr int64_t kPlaybackTransitionHoldMs = 500;
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;
using winrt::Windows::Data::Json::JsonValueType;

struct PlaybackEndpoint {
  const wchar_t* source;
  const wchar_t* url;
  int64_t pollIntervalMs;
};

constexpr PlaybackEndpoint kPlaybackEndpoints[] = {
    {L"a", L"https://skrzk.pages.dev/api/playback?channel=buddies", 10 * 60'000},
    {L"b", L"https://skrzk.pages.dev/api/playback?channel=buddy46", 60 * 60'000},
};

double FirstNumber(const JsonObject& object, std::initializer_list<const wchar_t*> names) {
  for (const wchar_t* name : names) {
    const double value = json::Number(object, name, std::numeric_limits<double>::quiet_NaN());
    if (std::isfinite(value)) return value;
  }
  return 0;
}

double FirstNumberOr(const JsonObject& object, std::initializer_list<const wchar_t*> names,
                     double fallback) {
  for (const wchar_t* name : names) {
    const double value = json::Number(object, name, std::numeric_limits<double>::quiet_NaN());
    if (std::isfinite(value)) return value;
  }
  return fallback;
}

double FirstNumberAcross(const JsonObject& first, const JsonObject& second,
                         const JsonObject& third,
                         std::initializer_list<const wchar_t*> names,
                         double fallback = 0) {
  const double missing = std::numeric_limits<double>::quiet_NaN();
  const double firstValue = FirstNumberOr(first, names, missing);
  if (std::isfinite(firstValue)) return firstValue;
  const double secondValue = FirstNumberOr(second, names, missing);
  if (std::isfinite(secondValue)) return secondValue;
  return FirstNumberOr(third, names, fallback);
}

std::wstring FirstText(const JsonObject& object, std::initializer_list<const wchar_t*> names) {
  for (const wchar_t* name : names) {
    std::wstring value = json::Text(object, name);
    if (!value.empty()) return value;
  }
  return {};
}

JsonObject FirstObject(const JsonObject& object,
                       std::initializer_list<const wchar_t*> names) {
  for (const wchar_t* name : names) {
    JsonObject value = json::Object(object, name);
    if (value.Size() != 0) return value;
  }
  return JsonObject{};
}

std::optional<bool> FirstBoolean(
    const JsonObject& object, std::initializer_list<const wchar_t*> names) {
  for (const wchar_t* name : names) {
    try {
      if (object.HasKey(name) &&
          object.GetNamedValue(name).ValueType() == JsonValueType::Boolean) {
        return object.GetNamedBoolean(name);
      }
    } catch (...) {
    }
  }
  return std::nullopt;
}

std::optional<bool> FirstBooleanAcross(
    const JsonObject& first, const JsonObject& second, const JsonObject& third,
    std::initializer_list<const wchar_t*> names) {
  if (const auto value = FirstBoolean(first, names)) return value;
  if (const auto value = FirstBoolean(second, names)) return value;
  return FirstBoolean(third, names);
}

std::optional<int> FirstInteger(
    const JsonObject& object, std::initializer_list<const wchar_t*> names) {
  for (const wchar_t* name : names) {
    try {
      if (!object.HasKey(name) ||
          object.GetNamedValue(name).ValueType() != JsonValueType::Number) {
        continue;
      }
      const double value = object.GetNamedNumber(name);
      if (!std::isfinite(value) ||
          value < static_cast<double>(std::numeric_limits<int>::min()) ||
          value > static_cast<double>(std::numeric_limits<int>::max())) {
        continue;
      }
      return static_cast<int>(std::trunc(value));
    } catch (...) {
    }
  }
  return std::nullopt;
}

std::optional<int> FirstIntegerAcross(
    const JsonObject& first, const JsonObject& second, const JsonObject& third,
    std::initializer_list<const wchar_t*> names) {
  if (const auto value = FirstInteger(first, names)) return value;
  if (const auto value = FirstInteger(second, names)) return value;
  return FirstInteger(third, names);
}

struct PlaybackContractState {
  bool queuePlayingPresent = false;
  bool queuePlaying = false;
  bool topLevelPlayingPresent = false;
  bool topLevelPlaying = false;
  bool broadcasting = false;
  bool paused = false;
  bool ended = false;
  bool stale = false;
  bool setupRequired = false;
  bool currentIndexPresent = false;
  int currentIndex = -1;
  int currentMarkerIndex = -1;
  int queueSize = 0;
};

constexpr int ResolveContractCurrentIndex(const PlaybackContractState& state) {
  if (state.queueSize <= 0 || state.ended || state.setupRequired) return -1;
  if (state.currentIndexPresent) {
    return state.currentIndex >= 0 && state.currentIndex < state.queueSize
        ? state.currentIndex
        : -1;
  }
  if (state.currentMarkerIndex >= 0 && state.currentMarkerIndex < state.queueSize) {
    return state.currentMarkerIndex;
  }
  const bool legacyPlaying = state.queuePlayingPresent
      ? state.queuePlaying
      : (state.topLevelPlayingPresent ? state.topLevelPlaying : state.broadcasting);
  return legacyPlaying ? 0 : -1;
}

constexpr bool ResolveContractPlaying(const PlaybackContractState& state,
                                      int currentIndex) {
  if (currentIndex < 0 || state.paused || state.ended || state.stale ||
      state.setupRequired) {
    return false;
  }
  if (state.queuePlayingPresent) return state.queuePlaying;
  if (state.topLevelPlayingPresent) return state.topLevelPlaying;
  return state.broadcasting;
}

constexpr PlaybackContractState EndedQueueContractSample() {
  PlaybackContractState state;
  state.queuePlayingPresent = true;
  state.queuePlaying = false;
  state.topLevelPlayingPresent = true;
  state.topLevelPlaying = false;
  state.broadcasting = true;
  state.ended = true;
  state.stale = true;
  state.currentIndexPresent = true;
  state.currentIndex = -1;
  state.queueSize = 60;
  return state;
}

constexpr PlaybackContractState ExplicitNoCurrentContractSample() {
  PlaybackContractState state;
  state.topLevelPlayingPresent = true;
  state.topLevelPlaying = true;
  state.broadcasting = true;
  state.currentIndexPresent = true;
  state.currentIndex = -1;
  state.queueSize = 3;
  return state;
}

constexpr PlaybackContractState LegacyBroadcastContractSample() {
  PlaybackContractState state;
  state.broadcasting = true;
  state.queueSize = 3;
  return state;
}

static_assert(ResolveContractCurrentIndex(EndedQueueContractSample()) == -1);
static_assert(!ResolveContractPlaying(EndedQueueContractSample(), -1));
static_assert(ResolveContractCurrentIndex(ExplicitNoCurrentContractSample()) == -1);
static_assert(!ResolveContractPlaying(ExplicitNoCurrentContractSample(), -1));
static_assert(ResolveContractCurrentIndex(LegacyBroadcastContractSample()) == 0);
static_assert(ResolveContractPlaying(LegacyBroadcastContractSample(), 0));

int64_t EpochMilliseconds(double value) {
  if (!std::isfinite(value) || value <= 0) return 0;



  if (value < 100'000'000'000.0) value *= 1000.0;
  return static_cast<int64_t>(value);
}

JsonObject UnwrapPlaybackPayload(const JsonObject& root) {
  JsonObject resolved = json::Object(root, L"resolved");
  if (resolved.Size() != 0) return resolved;

  JsonObject value = root;
  for (const wchar_t* name : {L"playback", L"data", L"stationhead", L"result"}) {
    JsonObject child = json::Object(value, name);
    if (child.Size() == 0) continue;
    value = child;
    if (_wcsicmp(name, L"data") == 0) {
      JsonObject nested = json::Object(value, L"playback");
      if (nested.Size() != 0) value = nested;
    }
    break;
  }
  return value;
}

struct PlaybackQueueSource {
  JsonObject owner;
  JsonArray queue;
};

PlaybackQueueSource LocatePlaybackQueue(const JsonObject& value) {
  PlaybackQueueSource result{value, json::Array(value, L"queue")};
  if (result.queue.Size() != 0) return result;

  const JsonObject currentStation = FirstObject(value, {L"currentStation", L"current_station"});
  if (currentStation.Size() != 0) {
    result.queue = json::Array(currentStation, L"queue");
    if (result.queue.Size() != 0) {
      result.owner = currentStation;
      return result;
    }
  }

  const JsonObject channel = json::Object(value, L"channel");
  if (channel.Size() != 0) {
    const JsonObject channelStation = FirstObject(channel, {L"currentStation", L"current_station"});
    if (channelStation.Size() != 0) {
      result.queue = json::Array(channelStation, L"queue");
      if (result.queue.Size() != 0) {
        result.owner = channelStation;
        return result;
      }
    }
    result.queue = json::Array(channel, L"queue");
    if (result.queue.Size() != 0) {
      result.owner = channel;
      return result;
    }
  }

  result.owner = value;
  return result;
}

JsonObject LocateQueueStatus(const JsonObject& value, const JsonObject& owner) {
  JsonObject status = FirstObject(owner, {L"queue_status", L"queueStatus"});
  if (status.Size() == 0) status = FirstObject(value, {L"queue_status", L"queueStatus"});
  return status;
}

JsonObject LocateCurrentItem(const JsonObject& value, const JsonObject& owner) {
  JsonObject item = FirstObject(value, {L"item", L"currentItem", L"currentTrack", L"track", L"song"});
  if (item.Size() == 0 && owner.Size() != 0) {
    item = FirstObject(owner, {L"item", L"currentItem", L"currentTrack", L"track", L"song"});
  }
  if (item.Size() == 0 &&
      (!FirstText(value, {L"name", L"title", L"trackTitle", L"display_title"}).empty() ||
       json::Boolean(value, L"hasTrack"))) {
    item = value;
  }
  return item;
}

NativePlaybackTrack NormalizeTrack(const fs::path& dataDir, const JsonObject& raw) {
  JsonObject source = raw;
  JsonObject nested = json::Object(raw, L"track");
  if (nested.Size() == 0) nested = json::Object(raw, L"song");
  if (nested.Size() != 0) source = nested;

  NativePlaybackTrack track;
  track.title = FirstText(source, {L"name", L"title", L"trackTitle", L"display_title", L"spotify_id"});

  std::wstring artist = FirstText(source, {L"trackArtist", L"artist"});
  if (artist.empty()) {
    const JsonObject artistObject = json::Object(source, L"artist");
    artist = json::Text(artistObject, L"name");
  }
  if (artist.empty()) {
    const JsonArray artists = json::Array(source, L"artists");
    std::wstring joined;
    for (uint32_t index = 0; index < artists.Size(); ++index) {
      try {
        std::wstring part;
        const auto value = artists.GetAt(index);
        if (value.ValueType() == JsonValueType::String) part = value.GetString().c_str();
        else if (value.ValueType() == JsonValueType::Object) {
          part = json::Text(value.GetObject(), L"name");
        }
        if (part.empty()) continue;
        if (!joined.empty()) joined += L", ";
        joined += part;
      } catch (...) {
      }
    }
    artist = joined;
  }
  track.artist = artist;

  std::wstring artwork = FirstText(source, {
      L"artwork", L"artworkUrl", L"albumArtUrl", L"image", L"imageUrl",
      L"thumbnail_url", L"thumbnailUrl",
  });
  if (artwork.empty()) {
    JsonObject album = json::Object(source, L"album");
    JsonArray images = json::Array(album, L"images");
    if (images.Size() == 0) images = json::Array(source, L"images");
    if (images.Size() > 0) {
      try {
        if (images.GetAt(0).ValueType() == JsonValueType::Object) {
          artwork = json::Text(images.GetAt(0).GetObject(), L"url");
        }
      } catch (...) {
      }
    }
  }
  track.artwork = CacheArtworkUrl(dataDir, artwork);
  track.durationMs = static_cast<int64_t>(std::max(
      0.0, FirstNumber(source, {L"durationMs", L"duration_ms", L"lengthMs", L"duration"})));
  return track;
}

NativePlaybackProjection ParsePlaybackProjection(const fs::path& dataDir,
                                                  const std::wstring& payload,
                                                  int64_t fetchedAt) {
  NativePlaybackProjection projection;
  projection.fetchedAt = fetchedAt;
  if (payload.empty()) return projection;
  try {
    const JsonObject root = JsonObject::Parse(payload);
    const JsonObject value = UnwrapPlaybackPayload(root);
    const PlaybackQueueSource queueSource = LocatePlaybackQueue(value);
    const JsonObject queueOwner = queueSource.owner;
    const JsonArray queue = queueSource.queue;
    const JsonObject queueStatus = LocateQueueStatus(value, queueOwner);
    const JsonObject itemSource = LocateCurrentItem(value, queueOwner);

    const std::optional<bool> queuePlaying = FirstBoolean(queueStatus, {L"playing"});
    std::optional<bool> topLevelPlaying = FirstBoolean(value, {L"playing"});
    if (!topLevelPlaying) topLevelPlaying = FirstBoolean(queueOwner, {L"playing"});
    const bool statusPaused = FirstBooleanAcross(
        queueStatus, queueOwner, value, {L"is_paused", L"isPaused"}).value_or(false);
    const bool broadcasting = FirstBooleanAcross(
        value, queueOwner, queueStatus,
        {L"is_broadcasting", L"isBroadcasting"}).value_or(false);
    const bool ended = FirstBooleanAcross(
        queueStatus, queueOwner, value, {L"ended"}).value_or(false);
    const bool stale = FirstBooleanAcross(
        value, queueOwner, queueStatus, {L"stale"}).value_or(false);
    const bool setupRequired = FirstBooleanAcross(
        value, queueOwner, queueStatus,
        {L"setup_required", L"setupRequired"}).value_or(false);

    std::optional<int> explicitCurrentIndex = FirstIntegerAcross(
        queueStatus, queueOwner, value, {L"current_index", L"currentIndex"});
    int currentMarkerIndex = -1;
    for (uint32_t queueIndex = 0; queueIndex < queue.Size(); ++queueIndex) {
      try {
        if (queue.GetAt(queueIndex).ValueType() == JsonValueType::Object &&
            json::Boolean(queue.GetAt(queueIndex).GetObject(), L"is_current")) {
          currentMarkerIndex = static_cast<int>(queueIndex);
          break;
        }
      } catch (...) {
      }
    }
    if (queue.Size() == 0 && itemSource.Size() != 0) currentMarkerIndex = 0;

    const int contractQueueSize = queue.Size() > 0
        ? static_cast<int>(std::min<uint32_t>(
              queue.Size(), static_cast<uint32_t>(std::numeric_limits<int>::max())))
        : (itemSource.Size() != 0 ? 1 : 0);
    PlaybackContractState contract;
    contract.queuePlayingPresent = queuePlaying.has_value();
    contract.queuePlaying = queuePlaying.value_or(false);
    contract.topLevelPlayingPresent = topLevelPlaying.has_value();
    contract.topLevelPlaying = topLevelPlaying.value_or(false);
    contract.broadcasting = broadcasting;
    contract.paused = statusPaused;
    contract.ended = ended;
    contract.stale = stale;
    contract.setupRequired = setupRequired;
    contract.currentIndexPresent = explicitCurrentIndex.has_value();
    contract.currentIndex = explicitCurrentIndex.value_or(-1);
    contract.currentMarkerIndex = currentMarkerIndex;
    contract.queueSize = contractQueueSize;

    const int currentIndex = ResolveContractCurrentIndex(contract);
    const bool playing = ResolveContractPlaying(contract, currentIndex);

    const int64_t sampledAt = EpochMilliseconds(FirstNumberAcross(
        value, queueOwner, queueStatus,
        {L"generated_at", L"queue_observed_at", L"latest_observed_at",
         L"sampledAt", L"monitorSampledAt", L"updatedAt"}));
    const int64_t serverReferenceAt = EpochMilliseconds(FirstNumberAcross(
        value, queueOwner, queueStatus,
        {L"generated_at", L"queue_observed_at", L"latest_observed_at",
         L"sampledAt", L"monitorSampledAt", L"updatedAt"}));
    const int64_t anchorAt = EpochMilliseconds(FirstNumberAcross(
        value, queueOwner, queueStatus, {L"anchor_at", L"anchorAt"}));
    const int64_t queueEndAt = EpochMilliseconds(FirstNumberAcross(
        value, queueOwner, queueStatus, {L"queue_end_at", L"queueEndAt"}));
    int64_t durationMs = static_cast<int64_t>(std::max(
        0.0, FirstNumberAcross(value, queueOwner, queueStatus,
                               {L"duration_ms", L"durationMs", L"trackDurationMs"})));
    int64_t progressMs = static_cast<int64_t>(std::max(
        0.0, FirstNumberAcross(value, queueOwner, queueStatus,
                               {L"progress_ms", L"progressMs", L"positionMs"})));

    if (queue.Size() > 0) {
      projection.queue.reserve(queue.Size());
      for (uint32_t queueIndex = 0; queueIndex < queue.Size(); ++queueIndex) {
        try {
          if (queue.GetAt(queueIndex).ValueType() == JsonValueType::Object) {
            projection.queue.push_back(
                NormalizeTrack(dataDir, queue.GetAt(queueIndex).GetObject()));
            continue;
          }
        } catch (...) {
        }
        projection.queue.emplace_back();
      }

      if (currentIndex >= 0 && currentIndex < static_cast<int>(queue.Size())) {
        if (progressMs <= 0) {
          try {
            if (queue.GetAt(currentIndex).ValueType() == JsonValueType::Object) {
              progressMs = static_cast<int64_t>(std::max(
                  0.0, FirstNumber(queue.GetAt(currentIndex).GetObject(),
                                   {L"progress_ms", L"progressMs", L"positionMs"})));
            }
          } catch (...) {
          }
        }
        if (durationMs <= 0 && currentIndex < static_cast<int>(projection.queue.size())) {
          durationMs = projection.queue[static_cast<size_t>(currentIndex)].durationMs;
        }

        int64_t elapsed = progressMs;
        if (playing) {
          if (anchorAt > 0 && serverReferenceAt > 0) {
            elapsed = std::max<int64_t>(0, serverReferenceAt - anchorAt) +
                std::max<int64_t>(0, fetchedAt - serverReferenceAt);
          } else if (anchorAt > 0) {
            elapsed = std::max<int64_t>(0, fetchedAt - anchorAt);
          } else if (sampledAt > 0) {
            elapsed += std::max<int64_t>(0, fetchedAt - sampledAt);
          }
        }
        projection.progressMs = std::max<int64_t>(0, elapsed);
      }
      projection.currentIndex = currentIndex;
    } else if (itemSource.Size() != 0) {
      NativePlaybackTrack track = NormalizeTrack(dataDir, itemSource);
      if (durationMs > 0) track.durationMs = durationMs;
      if (currentIndex >= 0) {
        const int64_t expectedEndAt = EpochMilliseconds(FirstNumberAcross(
            value, queueOwner, queueStatus, {L"expected_end_at", L"expectedEndAt"}));
        if (track.durationMs > 0 && expectedEndAt > 0) {
          progressMs = track.durationMs -
              std::max<int64_t>(0, expectedEndAt + kPlaybackTransitionHoldMs - fetchedAt);
        } else if (playing && sampledAt > 0) {
          progressMs += std::max<int64_t>(0, fetchedAt - sampledAt);
        }
        if (track.durationMs > 0) {
          progressMs = std::clamp<int64_t>(progressMs, 0, track.durationMs);
        }
        projection.currentIndex = 0;
        projection.progressMs = std::max<int64_t>(0, progressMs);
      }
      projection.queue.push_back(std::move(track));
    }

    const bool hasTrackData = std::any_of(
        projection.queue.begin(), projection.queue.end(),
        [](const NativePlaybackTrack& track) {
          return !track.title.empty() || !track.artist.empty() ||
              !track.artwork.empty() || track.durationMs > 0;
        });
    projection.available = hasTrackData || stale || ended || setupRequired;
    projection.playing = playing;
    projection.stale = stale;
    projection.ended = ended;
    projection.setupRequired = setupRequired;
    projection.queueRevision = FirstText(value, {L"queue_revision", L"queueRevision"});
    if (projection.queueRevision.empty()) {
      projection.queueRevision =
          FirstText(queueOwner, {L"queue_revision", L"queueRevision"});
    }
    projection.sampledAt = fetchedAt;
    projection.anchorAt = playing ? fetchedAt - projection.progressMs : 0;
    if (queueEndAt > 0 && serverReferenceAt > 0) {
      projection.queueEndAt = fetchedAt + std::max<int64_t>(0, queueEndAt - serverReferenceAt);
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

std::wstring FetchPlaybackJson(const wchar_t* rawUrl, std::wstring* payload) {
  if (!rawUrl || !*rawUrl || !payload) return L"playback URL missing";
  payload->clear();

  std::wstring requestUrl(rawUrl);
  requestUrl += requestUrl.find(L'?') == std::wstring::npos ? L"?" : L"&";
  requestUrl += L"_hp=" + std::to_wstring(UnixMillis());

  std::vector<uint8_t> body;
  std::wstring error;
  if (!WinHttpDownload(requestUrl, kMaxPlaybackResponseBytes, &body, nullptr, &error,
                       L"HomePanel-Native-Playback/1.1",
                       L"Accept: application/json\r\nCache-Control: no-cache, no-store\r\nPragma: no-cache\r\n")) {
    return error.empty() ? L"playback download failed" : error;
  }

  const std::wstring wide = Utf8ToWide(std::string(body.begin(), body.end()));
  if (wide.empty()) return L"invalid UTF-8 playback response";
  try {
    const auto parsed = JsonValue::Parse(wide);
    if (parsed.ValueType() != JsonValueType::Object) {
      return L"playback response is not a JSON object";
    }
    const JsonObject root = parsed.GetObject();
    if (!json::Boolean(root, L"ok", true)) {
      const std::wstring detail = json::Text(root, L"error", L"playback API returned ok=false");
      return detail;
    }
  } catch (...) {
    return L"invalid playback JSON";
  }

  *payload = wide;
  return {};
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
  NativePlaybackProjection projection;
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
    const std::wstring payload = json::Stringify(root, L"payload");
    const std::wstring error = json::Text(root, L"error");
    const int64_t fetchedAt = static_cast<int64_t>(std::max(
        0.0, json::Number(root, L"fetchedAt")));
    snapshot->source = source;
    snapshot->payload = payload;
    snapshot->error = error;
    snapshot->fetchedAt = fetchedAt;
    snapshot->projection = ParsePlaybackProjection(dataDir, payload, fetchedAt);
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
      update.projection = std::move(loaded.projection);
      update.hasPayload = loaded.hasPayload;
      update.contentRevision = ++nativePlaybackContentRevision_;
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
  const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  std::array<int64_t, std::size(kPlaybackEndpoints)> nextPollAt{};
  while (!nativePlaybackStopping_.load(std::memory_order_acquire)) {
    const int64_t nowMs = UnixMillis();
    for (size_t index = 0; index < std::size(kPlaybackEndpoints); ++index) {
      if (nativePlaybackStopping_.load(std::memory_order_acquire)) break;
      if (nextPollAt[index] > nowMs) continue;
      std::wstring payload;
      const int64_t fetchedAt = UnixMillis();
      const std::wstring error = FetchPlaybackJson(kPlaybackEndpoints[index].url, &payload);
      NativePlaybackProjection projection =
          ParsePlaybackProjection(dataDir_, payload, fetchedAt);
      SaveNativePlaybackSnapshot(
          dataDir_, kPlaybackEndpoints[index].source, payload, error, fetchedAt);
      {
        std::lock_guard lock(nativePlaybackMutex_);
        auto& update = nativePlaybackUpdates_[index];
        update.source = kPlaybackEndpoints[index].source;
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
      nextPollAt[index] = UnixMillis() + kPlaybackEndpoints[index].pollIntervalMs;
    }

    const int64_t waitStartedAt = UnixMillis();
    int64_t nextDueAt = waitStartedAt + 60 * 60'000;
    for (const int64_t pollAt : nextPollAt) {
      if (pollAt > 0) nextDueAt = std::min(nextDueAt, pollAt);
    }
    std::unique_lock waitLock(nativePlaybackWakeMutex_);
    nativePlaybackWake_.wait_for(
        waitLock,
        std::chrono::milliseconds(std::max<int64_t>(
            1'000, nextDueAt - waitStartedAt)),
        [this] { return nativePlaybackStopping_.load(std::memory_order_acquire); });
  }
  if (SUCCEEDED(apartment)) CoUninitialize();
}

}  // namespace hp
