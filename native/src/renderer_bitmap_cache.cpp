#include "web_renderer.h"
#include "wic_image.h"

namespace hp {
namespace {
constexpr size_t kNativeImageBitmapCacheLimit = 16;
constexpr size_t kRadarBitmapCacheLimit = 12;
constexpr int64_t kNativeImageDecodeRetryMs = 60'000;

struct CachedBitmapMemoryDc {
  HDC value = nullptr;
  ~CachedBitmapMemoryDc() {
    if (value) DeleteDC(value);
  }
};

HDC BitmapMemoryDc(HDC compatibleDc) {
  thread_local CachedBitmapMemoryDc cached;
  if (!cached.value) cached.value = CreateCompatibleDC(compatibleDc);
  return cached.value;
}

HBRUSH BitmapCacheBackgroundBrush() {
  static HBRUSH brush = CreateSolidBrush(kNativeDashboardBackground);
  return brush;
}

bool IsPersistentRadarBitmap(const std::wstring& key) {
  return key.rfind(L"radar-satellite#", 0) == 0 ||
         key.rfind(L"radar-map#", 0) == 0;
}
}  // namespace

void Renderer::DrawCachedPanelSection(
    HDC dc, const RECT& card, PanelSection section, uint64_t revision,
    void (Renderer::*draw)(HDC, const RECT&)) {
  const int width = std::max(1, static_cast<int>(card.right - card.left));
  const int height = std::max(1, static_cast<int>(card.bottom - card.top));
  PanelBitmapCache& cache = nativeSectionBitmaps_[section];
  const bool stale = !cache.bitmap || cache.width != width || cache.height != height ||
      cache.revision != revision || cache.layoutRevision != nativeLayoutRevision_;
  HDC memoryDc = BitmapMemoryDc(dc);
  if (stale) {
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    if (!bitmap || !memoryDc) {
      if (bitmap) DeleteObject(bitmap);
      (this->*draw)(dc, card);
      return;
    }
    HGDIOBJ previous = SelectObject(memoryDc, bitmap);
    const RECT local{0, 0, width, height};
    FillRect(memoryDc, &local, BitmapCacheBackgroundBrush());
    SetBkMode(memoryDc, TRANSPARENT);
    (this->*draw)(memoryDc, local);
    SelectObject(memoryDc, previous);
    if (cache.bitmap) DeleteObject(cache.bitmap);
    cache = PanelBitmapCache{bitmap, width, height, revision, nativeLayoutRevision_};
  }
  if (!cache.bitmap || !memoryDc) {
    (this->*draw)(dc, card);
    return;
  }
  HGDIOBJ previous = SelectObject(memoryDc, cache.bitmap);
  BitBlt(dc, card.left, card.top, width, height, memoryDc, 0, 0, SRCCOPY);
  SelectObject(memoryDc, previous);
}

HBITMAP Renderer::NativePanelBackBuffer(HWND hwnd, HDC dc, int width, int height) {
  if (!hwnd || !dc || width <= 0 || height <= 0) return nullptr;
  PanelBackBuffer& buffer = nativeBackBuffers_[hwnd];
  if (buffer.bitmap && buffer.width == width && buffer.height == height) {
    return buffer.bitmap;
  }
  if (buffer.bitmap) DeleteObject(buffer.bitmap);
  buffer.bitmap = CreateCompatibleBitmap(dc, width, height);
  buffer.width = buffer.bitmap ? width : 0;
  buffer.height = buffer.bitmap ? height : 0;
  return buffer.bitmap;
}

void Renderer::ReleaseNativePanelBackBuffer(HWND hwnd) {
  const auto found = nativeBackBuffers_.find(hwnd);
  if (found == nativeBackBuffers_.end()) return;
  if (found->second.bitmap) DeleteObject(found->second.bitmap);
  nativeBackBuffers_.erase(found);
}

HBITMAP Renderer::NativeArtworkBitmap(const std::wstring& url, int width, int height) {
  if (url.empty() || width <= 0 || height <= 0) return nullptr;
  static constexpr wchar_t kDataHostPrefix[] = L"https://data.homepanel/";
  if (url.rfind(kDataHostPrefix, 0) != 0) return nullptr;

  const std::wstring key = url + L"#" + std::to_wstring(width) + L"x" +
      std::to_wstring(height);
  auto found = nativeImageBitmaps_.find(key);
  if (found != nativeImageBitmaps_.end()) {
    if (found->second.bitmap) {
      found->second.lastUsed = ++nativeImageUseCounter_;
      return found->second.bitmap;
    }
    // Null entries carry a retry deadline in lastUsed. This stops a missing
    // artwork file from causing a disk read and WIC decode on every repaint,
    // while still allowing a later download to become visible.
    if (UnixMillis() < static_cast<int64_t>(found->second.lastUsed)) return nullptr;
    nativeImageBitmaps_.erase(found);
  }

  std::wstring relative = url.substr(std::size(kDataHostPrefix) - 1);
  if (relative.empty() || relative.find(L"..") != std::wstring::npos) return nullptr;
  for (auto& character : relative) {
    if (character == L'/') character = L'\\';
  }
  return CacheNativeImageBitmap(
      key, DecodeImageFileToBitmap(dataDir_ / relative, width, height));
}

HBITMAP Renderer::NativeWeatherIconBitmap(
    const std::wstring& icon, bool night, int width, int height) {
  if (icon.empty() || width <= 0 || height <= 0) return nullptr;
  for (wchar_t character : icon) {
    if (character < L'0' || character > L'9') return nullptr;
  }
  const std::wstring fileName = icon + (night ? L"_night.png" : L"_day.png");
  const std::wstring key = L"weather-icon:" + fileName + L"#" +
      std::to_wstring(width) + L"x" + std::to_wstring(height);
  auto found = nativeImageBitmaps_.find(key);
  if (found != nativeImageBitmaps_.end()) {
    if (found->second.bitmap) {
      found->second.lastUsed = ++nativeImageUseCounter_;
      return found->second.bitmap;
    }
    if (UnixMillis() < static_cast<int64_t>(found->second.lastUsed)) return nullptr;
    nativeImageBitmaps_.erase(found);
  }

  const auto decodeIcon = [&](const std::wstring& name) {
    return DecodeImageFileToBitmap(
        rootDir_ / L"ui" / L"weather-icons" / name, width, height);
  };
  HBITMAP bitmap = decodeIcon(fileName);
  if (!bitmap && night) bitmap = decodeIcon(icon + L"_day.png");
  if (!bitmap) {
    const wchar_t family = icon.front();
    const std::wstring fallback = family == L'2' ? L"200" :
        family == L'3' ? L"300" : family == L'4' ? L"400" : L"100";
    bitmap = decodeIcon(fallback + (night ? L"_night.png" : L"_day.png"));
    if (!bitmap && night) bitmap = decodeIcon(fallback + L"_day.png");
  }
  return CacheNativeImageBitmap(key, bitmap);
}

HBITMAP Renderer::CacheNativeImageBitmap(const std::wstring& key, HBITMAP bitmap) {
  if (nativeImageBitmaps_.size() >= kNativeImageBitmapCacheLimit) {
    auto oldest = nativeImageBitmaps_.end();
    for (auto item = nativeImageBitmaps_.begin();
         item != nativeImageBitmaps_.end(); ++item) {
      // Prefer evicting a negative-cache entry before a decoded bitmap. This
      // keeps the backoff bounded without sacrificing useful image memory.
      if (!item->second.bitmap) {
        oldest = item;
        break;
      }
      if (oldest == nativeImageBitmaps_.end() ||
          item->second.lastUsed < oldest->second.lastUsed) {
        oldest = item;
      }
    }
    if (oldest != nativeImageBitmaps_.end()) {
      if (oldest->second.bitmap) DeleteObject(oldest->second.bitmap);
      nativeImageBitmaps_.erase(oldest);
    }
  }
  const uint64_t cacheStamp = bitmap
      ? ++nativeImageUseCounter_
      : static_cast<uint64_t>(UnixMillis() + kNativeImageDecodeRetryMs);
  nativeImageBitmaps_[key] = BitmapCacheEntry{bitmap, cacheStamp};
  return bitmap;
}

HBITMAP Renderer::CachedRadarBitmap(
    const std::wstring& key, const fs::path& path, const std::string& fileStamp,
    int width, int height) {
  if (width <= 0 || height <= 0) return nullptr;
  const std::wstring keyPrefix = key + L"#";
  const std::wstring cacheKey = keyPrefix + Utf8ToWide(fileStamp) + L"#" +
      std::to_wstring(width) + L"x" + std::to_wstring(height);
  auto found = nativeRadarBitmaps_.find(cacheKey);
  if (found != nativeRadarBitmaps_.end()) {
    found->second.lastUsed = ++nativeRadarBitmapUseCounter_;
    return found->second.bitmap;
  }

  HBITMAP bitmap = DecodeImageFileToBitmap(path, width, height);
  if (!bitmap) return nullptr;
  for (auto item = nativeRadarBitmaps_.begin(); item != nativeRadarBitmaps_.end();) {
    if (item->first.rfind(keyPrefix, 0) != 0) {
      ++item;
      continue;
    }
    if (item->second.bitmap) DeleteObject(item->second.bitmap);
    item = nativeRadarBitmaps_.erase(item);
  }
  if (nativeRadarBitmaps_.size() >= kRadarBitmapCacheLimit) {
    auto oldest = nativeRadarBitmaps_.end();
    for (auto item = nativeRadarBitmaps_.begin();
         item != nativeRadarBitmaps_.end(); ++item) {
      if (IsPersistentRadarBitmap(item->first)) continue;
      if (oldest == nativeRadarBitmaps_.end() ||
          item->second.lastUsed < oldest->second.lastUsed) {
        oldest = item;
      }
    }
    if (oldest != nativeRadarBitmaps_.end()) {
      if (oldest->second.bitmap) DeleteObject(oldest->second.bitmap);
      nativeRadarBitmaps_.erase(oldest);
    }
  }
  nativeRadarBitmaps_[cacheKey] =
      BitmapCacheEntry{bitmap, ++nativeRadarBitmapUseCounter_};
  return bitmap;
}

void Renderer::ReleaseNativePanelSurfaces() noexcept {
  const auto deleteBitmaps = [](auto& entries) {
    for (auto& item : entries) {
      if (item.second.bitmap) DeleteObject(item.second.bitmap);
    }
    entries.clear();
  };
  deleteBitmaps(nativeBackBuffers_);
  deleteBitmaps(nativeSectionBitmaps_);
}

void Renderer::ResetNativeBitmapCaches() noexcept {
  ReleaseNativePanelSurfaces();
  const auto deleteBitmaps = [](auto& entries) {
    for (auto& item : entries) {
      if (item.second.bitmap) DeleteObject(item.second.bitmap);
    }
    entries.clear();
  };
  deleteBitmaps(nativeImageBitmaps_);
  nativeImageUseCounter_ = 0;
  deleteBitmaps(nativeRadarBitmaps_);
  nativeRadarBitmapUseCounter_ = 0;
}

}  // namespace hp