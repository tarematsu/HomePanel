#include "web_renderer.h"
#include "file_utils.h"
#include "wic_image.h"
#include "json_helpers.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {
constexpr int64_t kRadarTileFailureTtlMs = 5 * 60'000;
constexpr int64_t kDefaultRadarFrameIntervalMs = 5'000;
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValueType;

struct RadarTile {
  std::wstring url;
  POINT destination{};
  fs::path path;
  std::string fileStamp;
};

std::wstring RadarTimeFromMillis(int64_t milliseconds) {
  if (milliseconds <= 0) return {};
  const time_t seconds = static_cast<time_t>(milliseconds / 1000);
  tm local{};
  if (localtime_s(&local, &seconds) != 0) return {};
  wchar_t text[16]{};
  swprintf_s(text, L"%02d:%02d", local.tm_hour, local.tm_min);
  return text;
}

std::optional<fs::path> RadarTilePath(const fs::path& dataDir,
                                      const std::wstring& url) {
  static constexpr wchar_t kDataHostPrefix[] = L"https://data.homepanel/";
  if (url.empty() || url.rfind(kDataHostPrefix, 0) != 0) return std::nullopt;
  std::wstring relative = url.substr(std::size(kDataHostPrefix) - 1);
  if (relative.empty() || relative.find(L"..") != std::wstring::npos) return std::nullopt;
  for (auto& character : relative) {
    if (character == L'/') character = L'\\';
  }
  return dataDir / relative;
}

bool TileFailureActive(const std::map<std::wstring, int64_t>& failures,
                       const std::wstring& url, int64_t now) {
  const auto item = failures.find(url);
  return item != failures.end() && item->second > now;
}

bool SaveBitmapAsBmp(HBITMAP bitmap, const fs::path& path, int width, int height) {
  if (!bitmap || width <= 0 || height <= 0) return false;
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;

  const size_t headerBytes = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
  const size_t pixelBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
  if (pixelBytes > static_cast<size_t>(MAXDWORD) - headerBytes) return false;
  std::vector<uint8_t> bytes(headerBytes + pixelBytes);
  uint8_t* const pixels = bytes.data() + headerBytes;

  HDC dc = GetDC(nullptr);
  if (!dc) return false;
  const int lines = GetDIBits(
      dc, bitmap, 0, static_cast<UINT>(height), pixels, &info, DIB_RGB_COLORS);
  ReleaseDC(nullptr, dc);
  if (lines != height) return false;

  BITMAPFILEHEADER file{};
  file.bfType = 0x4d42;
  file.bfOffBits = static_cast<DWORD>(headerBytes);
  file.bfSize = static_cast<DWORD>(bytes.size());
  std::memcpy(bytes.data(), &file, sizeof(file));
  std::memcpy(bytes.data() + sizeof(file), &info.bmiHeader, sizeof(BITMAPINFOHEADER));
  return AtomicWriteBytes(path, bytes);
}

void BlendBitmap(HDC dc, HBITMAP bitmap, int left, int top, int width, int height) {
  if (!bitmap || width <= 0 || height <= 0) return;
  HDC sourceDc = CreateCompatibleDC(dc);
  if (!sourceDc) return;
  HGDIOBJ previous = SelectObject(sourceDc, bitmap);
  const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  AlphaBlend(dc, left, top, width, height, sourceDc, 0, 0, width, height, blend);
  SelectObject(sourceDc, previous);
  DeleteDC(sourceDc);
}
}  // namespace

void Renderer::NotifyRadarUpdated() {
  if (!radarComposeStarted_.load(std::memory_order_acquire)) return;
  {
    std::lock_guard lock(radarComposeWakeMutex_);
    radarComposePending_ = true;
  }
  radarComposeWake_.notify_all();
}

void Renderer::StartRadarCompose() {
  if (radarComposeStarted_.exchange(true, std::memory_order_acq_rel)) return;
  radarComposeStopping_ = false;
  {
    std::lock_guard lock(radarComposeWakeMutex_);
    radarComposePending_ = true;
  }
  radarComposeThread_ = std::thread([this] { RadarComposeLoop(); });
}

void Renderer::StopRadarCompose() noexcept {
  if (!radarComposeStarted_.exchange(false, std::memory_order_acq_rel)) return;
  radarComposeStopping_ = true;
  radarComposeWake_.notify_all();
  if (radarComposeThread_.joinable()) radarComposeThread_.join();
}

void Renderer::RadarComposeLoop() {
  const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  while (!radarComposeStopping_.load(std::memory_order_acquire)) {
    {
      std::unique_lock waitLock(radarComposeWakeMutex_);
      radarComposeWake_.wait_for(waitLock, std::chrono::milliseconds(kDefaultRadarFrameIntervalMs), [this] {
        return radarComposePending_ ||
               radarComposeStopping_.load(std::memory_order_acquire);
      });
      if (radarComposeStopping_.load(std::memory_order_acquire)) break;
      radarComposePending_ = false;
    }
    try {
      ComposeRadarFrame();
    } catch (...) {
    }
  }
  if (SUCCEEDED(apartment)) CoUninitialize();
}

void Renderer::ComposeRadarFrame() {
  std::wstring json;
  try {
    std::ifstream input(dataDir_ / L"radar.json", std::ios::binary);
    if (input) {
      const std::string text((std::istreambuf_iterator<char>(input)), {});
      json = Utf8ToWide(text);
    }
  } catch (...) {
  }

  int sourceWidth = 480;
  int sourceHeight = 320;
  int64_t validAt = 0;
  bool precomposed = false;
  std::wstring signature;
  std::vector<RadarTile> tiles;
  const fs::path uiDir = rootDir_ / L"ui";
  const fs::path satellitePath = uiDir / L"radar-satellite.png";
  const fs::path mapPath = uiDir / L"radar-map.png";
  const std::string satelliteStamp = file::Stamp(satellitePath);
  const std::string mapStamp = file::Stamp(mapPath);
  if (!json.empty()) {
    try {
      const JsonObject root = JsonObject::Parse(json);
      sourceWidth = std::max(1, static_cast<int>(json::Number(root, L"width", 480)));
      sourceHeight = std::max(1, static_cast<int>(json::Number(root, L"height", 320)));
      precomposed = json::Boolean(root, L"precomposed");
      const int64_t frameIntervalMs = std::clamp<int64_t>(
          static_cast<int64_t>(json::Number(root, L"frameIntervalMs", kDefaultRadarFrameIntervalMs)),
          1'000, 60'000);
      const JsonArray frames = json::Array(root, L"frames");
      if (frames.Size() > 0) {
        const uint32_t selectedIndex = static_cast<uint32_t>(
            (UnixMillis() / frameIntervalMs) % static_cast<int64_t>(frames.Size()));
        if (frames.GetAt(selectedIndex).ValueType() == JsonValueType::Object) {
          const JsonObject frame = frames.GetAt(selectedIndex).GetObject();
          validAt = static_cast<int64_t>(std::max(0.0, json::Number(frame, L"validAt")));
          const JsonArray frameTiles = json::Array(frame, L"tiles");
          std::wostringstream signatureStream;
          signatureStream << L"native-radar-v7|" << kRadarCanvasWidth << L'x' << kRadarCanvasHeight
                          << L"|source:" << sourceWidth << L'x' << sourceHeight
                          << L"|precomposed:" << (precomposed ? 1 : 0)
                          << L"|frame:" << selectedIndex << L'/' << frames.Size()
                          << L"|" << json::Text(frame, L"baseTime")
                          << L"|" << json::Text(frame, L"validTime")
                          << L"|" << validAt;
          if (!precomposed) {
            signatureStream << L"|sat:" << Utf8ToWide(satelliteStamp)
                            << L"|map:" << Utf8ToWide(mapStamp);
          }
          signatureStream << L"|tiles:" << frameTiles.Size();
          for (uint32_t index = 0; index < frameTiles.Size(); ++index) {
            if (frameTiles.GetAt(index).ValueType() != JsonValueType::Object) continue;
            const JsonObject tile = frameTiles.GetAt(index).GetObject();
            const std::wstring url = json::Text(tile, L"url");
            const POINT destination{
                static_cast<LONG>(json::Number(tile, L"destX")),
                static_cast<LONG>(json::Number(tile, L"destY")),
            };
            const std::optional<fs::path> tilePath = RadarTilePath(dataDir_, url);
            const std::string tileStamp = tilePath ? file::Stamp(*tilePath) : "invalid";
            tiles.push_back(RadarTile{
                url, destination, tilePath.value_or(fs::path{}), tileStamp});
            signatureStream << L"|" << url << L"@" << destination.x << L"," << destination.y
                            << L"#" << Utf8ToWide(tileStamp);
          }
          signature = signatureStream.str();
        }
      }
    } catch (...) {
      tiles.clear();
      signature.clear();
      validAt = 0;
      precomposed = false;
    }
  }

  {
    std::lock_guard lock(radarFrameMutex_);
    if (!signature.empty() && signature == radarSignature_ && radarFrameBitmap_) return;
  }

  const std::string signatureUtf8 = WideToUtf8(signature);
  const fs::path cachedFrame = dataDir_ / L"radar-frame.bmp";
  const fs::path cachedSignature = dataDir_ / L"radar-frame.signature";
  if (!signature.empty() && file::MatchesText(cachedSignature, signatureUtf8)) {
    HBITMAP cached = DecodeImageFileToBitmap(cachedFrame, kRadarCanvasWidth, kRadarCanvasHeight);
    if (cached) {
      std::wstring timeText = RadarTimeFromMillis(validAt);
      if (timeText.empty()) timeText = tiles.empty() ? L"待機中" : L"--:--";
      HBITMAP previousFrame = nullptr;
      {
        std::lock_guard lock(radarFrameMutex_);
        if (signature == radarSignature_ && radarFrameBitmap_) {
          DeleteObject(cached);
          return;
        }
        previousFrame = radarFrameBitmap_;
        radarFrameBitmap_ = cached;
        radarTimeText_ = std::move(timeText);
        radarSignature_ = signature;
      }
      if (previousFrame) DeleteObject(previousFrame);
      InvalidateAllNativePanels();
      return;
    }
  }

  if (precomposed &&
      (tiles.size() != 1 || tiles.front().destination.x != 0 || tiles.front().destination.y != 0)) {
    return;
  }

  HBITMAP radarSatelliteBitmap = nullptr;
  HBITMAP radarMapBitmap = nullptr;
  if (!precomposed) {
    radarSatelliteBitmap = CachedRadarBitmap(
        L"radar-satellite", satellitePath, satelliteStamp,
        kRadarCanvasWidth, kRadarCanvasHeight);
    radarMapBitmap = CachedRadarBitmap(
        L"radar-map", mapPath, mapStamp, kRadarCanvasWidth, kRadarCanvasHeight);
    if (!radarSatelliteBitmap || !radarMapBitmap) return;
  }

  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = kRadarCanvasWidth;
  info.bmiHeader.biHeight = -kRadarCanvasHeight;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* pixels = nullptr;
  HBITMAP composed = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
  if (!composed) return;

  HDC composeDc = CreateCompatibleDC(nullptr);
  if (!composeDc) {
    DeleteObject(composed);
    return;
  }
  HGDIOBJ previousComposed = SelectObject(composeDc, composed);
  if (precomposed) {
    PatBlt(composeDc, 0, 0, kRadarCanvasWidth, kRadarCanvasHeight, BLACKNESS);
  } else {
    BlendBitmap(composeDc, radarSatelliteBitmap, 0, 0, kRadarCanvasWidth, kRadarCanvasHeight);
  }

  const double scaleX = static_cast<double>(kRadarCanvasWidth) / sourceWidth;
  const double scaleY = static_cast<double>(kRadarCanvasHeight) / sourceHeight;
  const int tileWidth = static_cast<int>(std::ceil(256 * scaleX));
  const int tileHeight = static_cast<int>(std::ceil(256 * scaleY));
  size_t loadedTiles = 0;
  const int64_t now = UnixMillis();
  std::erase_if(radarFailedTiles_, [now](const auto& item) { return item.second <= now; });
  for (const RadarTile& tile : tiles) {
    if (radarComposeStopping_.load(std::memory_order_acquire)) break;
    if (TileFailureActive(radarFailedTiles_, tile.url, now)) continue;
    HBITMAP tileBitmap = tile.path.empty()
        ? nullptr
        : CachedRadarBitmap(L"radar-tile:" + tile.url, tile.path,
                            tile.fileStamp, tileWidth, tileHeight);
    if (!tileBitmap) {
      radarFailedTiles_[tile.url] = now + kRadarTileFailureTtlMs;
      continue;
    }
    BlendBitmap(composeDc, tileBitmap,
                static_cast<int>(std::lround(tile.destination.x * scaleX)),
                static_cast<int>(std::lround(tile.destination.y * scaleY)),
                tileWidth, tileHeight);
    ++loadedTiles;
  }

  if (!precomposed) {
    BlendBitmap(composeDc, radarMapBitmap, 0, 0, kRadarCanvasWidth, kRadarCanvasHeight);
  }
  SelectObject(composeDc, previousComposed);
  DeleteDC(composeDc);

  if (!tiles.empty() && loadedTiles == 0) {
    DeleteObject(composed);
    return;
  }

  std::wstring timeText = RadarTimeFromMillis(validAt);
  if (timeText.empty()) timeText = tiles.empty() ? L"待機中" : L"--:--";

  if (!signature.empty() && SaveBitmapAsBmp(composed, cachedFrame, kRadarCanvasWidth, kRadarCanvasHeight)) {
    AtomicWriteText(cachedSignature, signatureUtf8);
  }

  HBITMAP previousFrame = nullptr;
  {
    std::lock_guard lock(radarFrameMutex_);
    previousFrame = radarFrameBitmap_;
    radarFrameBitmap_ = composed;
    radarTimeText_ = std::move(timeText);
    radarSignature_ = signature;
  }
  if (previousFrame) DeleteObject(previousFrame);
  InvalidateAllNativePanels();
}
}  // namespace hp
