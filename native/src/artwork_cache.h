#pragma once

#include "common.h"
#include "winhttp_helpers.h"

namespace hp {
inline bool ContainsInsensitive(const std::wstring& value,
                                 const wchar_t* needle) noexcept {
  return needle && StrStrIW(value.c_str(), needle) != nullptr;
}

inline const wchar_t* ArtworkExtensionFromUrl(const std::wstring& url) noexcept {
  size_t end = url.find_first_of(L"?#");
  if (end == std::wstring::npos) end = url.size();
  if (end == 0) return nullptr;
  const size_t dot = url.rfind(L'.', end - 1);
  if (dot == std::wstring::npos) return nullptr;
  const size_t length = end - dot;
  const auto matches = [&](const wchar_t* extension) noexcept {
    const size_t expected = wcslen(extension);
    return length == expected &&
        _wcsnicmp(url.data() + dot, extension, expected) == 0;
  };
  if (matches(L".png")) return L".png";
  if (matches(L".jpg") || matches(L".jpeg")) return L".jpg";
  if (matches(L".webp")) return L".webp";
  if (matches(L".gif")) return L".gif";
  if (matches(L".bmp")) return L".bmp";
  return nullptr;
}

inline std::wstring GuessArtworkExtension(const std::wstring& contentType,
                                           const std::wstring& url) {
  if (ContainsInsensitive(contentType, L"image/png")) return L".png";
  if (ContainsInsensitive(contentType, L"image/webp")) return L".webp";
  if (ContainsInsensitive(contentType, L"image/gif")) return L".gif";
  if (ContainsInsensitive(contentType, L"image/bmp")) return L".bmp";
  if (ContainsInsensitive(contentType, L"image/jpeg") ||
      ContainsInsensitive(contentType, L"image/jpg")) {
    return L".jpg";
  }

  if (const wchar_t* extension = ArtworkExtensionFromUrl(url)) return extension;
  return L".img";
}

inline std::wstring CacheArtworkUrl(const fs::path& dataDir,
                                     const std::wstring& artworkUrl,
                                     const wchar_t* userAgent =
                                         L"HomePanel-Artwork-Cache/1.0") {
  if (artworkUrl.empty()) return {};
  if (artworkUrl.rfind(L"https://data.homepanel/", 0) == 0) return artworkUrl;

  constexpr int64_t kArtworkFailureRetryMs = 30 * 60'000;
  struct MemoryIndexEntry {
    std::wstring resolved;
    int64_t retryAfter = 0;
  };
  struct MemoryIndex {
    fs::path dataDir;
    std::map<std::wstring, MemoryIndexEntry> urls;
  };
  static thread_local MemoryIndex memoryIndex;
  if (memoryIndex.dataDir != dataDir) {
    memoryIndex.dataDir = dataDir;
    memoryIndex.urls.clear();
  }
  const int64_t now = UnixMillis();
  const auto indexed = memoryIndex.urls.find(artworkUrl);
  if (indexed != memoryIndex.urls.end()) {
    if (indexed->second.retryAfter <= 0 || now < indexed->second.retryAfter) {
      return indexed->second.resolved;
    }
    memoryIndex.urls.erase(indexed);
  }
  const auto remember = [&](std::wstring resolved, int64_t retryAfter = 0) {
    if (memoryIndex.urls.size() >= 128) {
      memoryIndex.urls.erase(memoryIndex.urls.begin());
    }
    memoryIndex.urls.insert_or_assign(
        artworkUrl, MemoryIndexEntry{std::move(resolved), retryAfter});
  };

  const std::string key = WideToUtf8(artworkUrl);
  if (key.empty()) return artworkUrl;

  const std::wstring stem = Hex64(Fnv1a64(key));
  const fs::path cacheDir = dataDir / L"spotify-artwork-cache";
  static thread_local fs::path preparedCacheDir;
  if (preparedCacheDir != cacheDir) {
    std::error_code error;
    fs::create_directories(cacheDir, error);
    preparedCacheDir = cacheDir;
  }

  std::wstring localUrl = L"https://data.homepanel/spotify-artwork-cache/";
  localUrl += stem;
  static constexpr const wchar_t* kExtensions[] = {
      L".jpg", L".png", L".webp", L".gif", L".bmp", L".img"};
  for (const wchar_t* extension : kExtensions) {
    const fs::path cached = cacheDir / (stem + extension);
    std::error_code itemError;
    const auto size = fs::file_size(cached, itemError);
    if (!itemError && size > 0) {
      const std::wstring resolved = localUrl + extension;
      remember(resolved);
      return resolved;
    }
  }

  std::vector<uint8_t> bytes;
  std::wstring contentType;
  if (!WinHttpDownload(artworkUrl.c_str(), 8 * 1024 * 1024, &bytes, &contentType,
                       nullptr, userAgent)) {
    remember(artworkUrl, now + kArtworkFailureRetryMs);
    return artworkUrl;
  }

  const std::wstring extension = GuessArtworkExtension(contentType, artworkUrl);
  const fs::path cached = cacheDir / (stem + extension);
  if (!AtomicWriteBytes(cached, bytes)) {
    remember(artworkUrl, now + kArtworkFailureRetryMs);
    return artworkUrl;
  }
  const std::wstring resolved = localUrl + extension;
  remember(resolved);
  return resolved;
}
}  // namespace hp
