#include "web_renderer.h"
#include "wic_image.h"
#include "json_helpers.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {
constexpr int64_t kRadarTileFailureTtlMs = 5 * 60'000;
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValueType;

struct RadarTile {
  std::wstring url;
  POINT destination{};
};

JsonArray RadarChildArray(const JsonObject& parent, const wchar_t* name) {
  return json::Array(parent, name);
}

std::wstring RadarText(const JsonObject& object, const wchar_t* name) {
  return json::Text(object, name);
}

double RadarNumber(const JsonObject& object, const wchar_t* name, double fallback = 0) {
  return json::Number(object, name, fallback);
}

std::wstring RadarTimeFromMillis(int64_t milliseconds) {
  if (milliseconds <= 0) return {};
  const time_t seconds = static_cast<time_t>(milliseconds / 1000);
  tm local{};
  if (localtime_s(&local, &seconds) != 0) return {};
  wchar_t text[16]{};
  swprintf_s(text, L"%02d:%02d", local.tm_hour, local.tm_min);
  return text;
}

HBITMAP DecodeRadarTile(const fs::path& dataDir, const std::wstring& url,
                        int width, int height) {
  static constexpr wchar_t kDataHostPrefix[] = L"https://data.homepanel/";
  if (url.empty()) return nullptr;
  if (url.rfind(kDataHostPrefix, 0) == 0) {
    std::wstring relative = url.substr(std::size(kDataHostPrefix) - 1);
    if (relative.empty() || relative.find(L"..") != std::wstring::npos) return nullptr;
    for (auto& character : relative) {
      if (character == L'/') character = L'\\';
    }
    return DecodeImageFileToBitmap(dataDir / relative, width, height);
  }
  return nullptr;
}

bool FileMatchesText(const fs::path& path, const std::string& content) {
  std::error_code error;
  if (!fs::is_regular_file(path, error) || fs::file_size(path, error) != content.size()) return false;
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  return std::equal(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>(),
                    content.begin(), content.end());
}

std::string FileStamp(const fs::path& path) {
  std::error_code error;
  std::ostringstream stamp;
  const auto size = fs::file_size(path, error);
  if (error) return "missing";
  stamp << size;
  const auto modified = fs::last_write_time(path, error);
  if (!error) stamp << ':' << modified.time_since_epoch().count();
  return stamp.str();
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

  std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
  HDC dc = GetDC(nullptr);
  if (!dc) return false;
  const int lines = GetDIBits(dc, bitmap, 0, static_cast<UINT>(height), pixels.data(), &info, DIB_RGB_COLORS);
  ReleaseDC(nullptr, dc);
  if (lines != height) return false;

  BITMAPFILEHEADER file{};
  file.bfType = 0x4d42;
  file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
  file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());

  std::vector<uint8_t> bytes(sizeof(file) + sizeof(BITMAPINFOHEADER) + pixels.size());
  std::memcpy(bytes.data(), &file, sizeof(file));
  std::memcpy(bytes.data() + sizeof(file), &info.bmiHeader, sizeof(BITMAPINFOHEADER));
  std::memcpy(bytes.data() + sizeof(file) + sizeof(BITMAPINFOHEADER), pixels.data(), pixels.size());
  return AtomicWriteBytes(path, bytes);
}

bool AtomicWriteTextBytes(const fs::path& path, const std::string& content) {
  return AtomicWriteBytes(path, std::vector<uint8_t>(content.begin(), content.end()));
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
      radarComposeWake_.wait(waitLock, [this] {
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

  int sourceWidth = 400;
  int sourceHeight = 260;
  int64_t validAt = 0;
  std::wstring signature;
  std::vector<RadarTile> tiles;
  // Bundled radar layers are extracted next to the executable under "ui" by
  // embedded_ui.cpp (executable.parent_path()/"ui"); rootDir_ is that exe dir.
  const fs::path uiDir = rootDir_ / L"ui";
  const fs::path satellitePath = uiDir / L"radar-satellite.png";
  const fs::path mapPath = uiDir / L"radar-map.png";
  if (!json.empty()) {
    try {
      const JsonObject root = JsonObject::Parse(json);
      sourceWidth = std::max(1, static_cast<int>(RadarNumber(root, L"width", 400)));
      sourceHeight = std::max(1, static_cast<int>(RadarNumber(root, L"height", 260)));
      const JsonArray frames = RadarChildArray(root, L"frames");
      if (frames.Size() > 0 && frames.GetAt(0).ValueType() == JsonValueType::Object) {
        const JsonObject frame = frames.GetAt(0).GetObject();
        validAt = static_cast<int64_t>(std::max(0.0, RadarNumber(frame, L"validAt")));
        const JsonArray frameTiles = RadarChildArray(frame, L"tiles");
        std::wostringstream signatureStream;
        signatureStream << L"native-radar-v2|" << sourceWidth << L'x' << sourceHeight
                        << L"|" << RadarText(frame, L"baseTime")
                        << L"|" << RadarText(frame, L"validTime")
                        << L"|" << validAt
                        << L"|sat:" << Utf8ToWide(FileStamp(satellitePath))
                        << L"|map:" << Utf8ToWide(FileStamp(mapPath))
                        << L"|tiles:" << frameTiles.Size();
        for (uint32_t index = 0; index < frameTiles.Size(); ++index) {
          if (frameTiles.GetAt(index).ValueType() != JsonValueType::Object) continue;
          const JsonObject tile = frameTiles.GetAt(index).GetObject();
          const std::wstring url = RadarText(tile, L"url");
          const POINT destination{
              static_cast<LONG>(RadarNumber(tile, L"destX")),
              static_cast<LONG>(RadarNumber(tile, L"destY")),
          };
          tiles.push_back(RadarTile{url, destination});
          signatureStream << L"|" << url << L"@" << destination.x << L"," << destination.y;
        }
        signature = signatureStream.str();
      }
    } catch (...) {
      tiles.clear();
      signature.clear();
      validAt = 0;
    }
  }

  {
    std::lock_guard lock(radarFrameMutex_);
    if (!signature.empty() && signature == radarSignature_ && radarFrameBitmap_) return;
  }

  const std::string signatureUtf8 = WideToUtf8(signature);
  const fs::path cachedFrame = dataDir_ / L"radar-frame.bmp";
  const fs::path cachedSignature = dataDir_ / L"radar-frame.signature";
  if (!signature.empty() && FileMatchesText(cachedSignature, signatureUtf8)) {
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
      const HWND radarWindow = nativeRadarWindow_;
      if (radarWindow && IsWindow(radarWindow)) InvalidateRect(radarWindow, nullptr, FALSE);
      InvalidateAllNativePanels();
      return;
    }
  }

  HBITMAP radarSatelliteBitmap = DecodeImageFileToBitmap(
      satellitePath, kRadarCanvasWidth, kRadarCanvasHeight);
  HBITMAP radarMapBitmap = DecodeImageFileToBitmap(
      mapPath, kRadarCanvasWidth, kRadarCanvasHeight);
  if (!radarSatelliteBitmap || !radarMapBitmap) {
    if (radarSatelliteBitmap) DeleteObject(radarSatelliteBitmap);
    if (radarMapBitmap) DeleteObject(radarMapBitmap);
    return;
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
  if (!composed) {
    DeleteObject(radarSatelliteBitmap);
    DeleteObject(radarMapBitmap);
    return;
  }

  HDC composeDc = CreateCompatibleDC(nullptr);
  if (!composeDc) {
    DeleteObject(composed);
    DeleteObject(radarSatelliteBitmap);
    DeleteObject(radarMapBitmap);
    return;
  }
  HGDIOBJ previousComposed = SelectObject(composeDc, composed);

  BlendBitmap(composeDc, radarSatelliteBitmap, 0, 0, kRadarCanvasWidth, kRadarCanvasHeight);

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
    HBITMAP tileBitmap = DecodeRadarTile(dataDir_, tile.url, tileWidth, tileHeight);
    if (!tileBitmap) {
      radarFailedTiles_[tile.url] = now + kRadarTileFailureTtlMs;
      continue;
    }
    BlendBitmap(composeDc, tileBitmap,
                static_cast<int>(std::lround(tile.destination.x * scaleX)),
                static_cast<int>(std::lround(tile.destination.y * scaleY)),
                tileWidth, tileHeight);
    DeleteObject(tileBitmap);
    ++loadedTiles;
  }

  BlendBitmap(composeDc, radarMapBitmap, 0, 0, kRadarCanvasWidth, kRadarCanvasHeight);
  SelectObject(composeDc, previousComposed);
  DeleteDC(composeDc);
  DeleteObject(radarSatelliteBitmap);
  DeleteObject(radarMapBitmap);

  if (!tiles.empty() && loadedTiles == 0) {
    // Keep the last successfully rendered frame when every tile fails.
    DeleteObject(composed);
    return;
  }

  std::wstring timeText = RadarTimeFromMillis(validAt);
  if (timeText.empty()) timeText = tiles.empty() ? L"待機中" : L"--:--";

  if (!signature.empty() && SaveBitmapAsBmp(composed, cachedFrame, kRadarCanvasWidth, kRadarCanvasHeight)) {
    AtomicWriteTextBytes(cachedSignature, signatureUtf8);
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

  const HWND radarWindow = nativeRadarWindow_;
  if (radarWindow && IsWindow(radarWindow)) InvalidateRect(radarWindow, nullptr, FALSE);
  InvalidateAllNativePanels();
}
}  // namespace hp
