#pragma once

#include "common.h"
#include "winhttp_helpers.h"

namespace hp {
inline std::wstring GuessArtworkExtension(const std::wstring& contentType,
                                          const std::wstring& url) {
  const std::wstring loweredType = [&] {
    std::wstring value = contentType;
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
  }();
  if (loweredType.find(L"image/png") != std::wstring::npos) return L".png";
  if (loweredType.find(L"image/webp") != std::wstring::npos) return L".webp";
  if (loweredType.find(L"image/gif") != std::wstring::npos) return L".gif";
  if (loweredType.find(L"image/bmp") != std::wstring::npos) return L".bmp";
  if (loweredType.find(L"image/jpeg") != std::wstring::npos ||
      loweredType.find(L"image/jpg") != std::wstring::npos) {
    return L".jpg";
  }

  const size_t query = url.find_first_of(L"?#");
  const std::wstring path = url.substr(0, query);
  const size_t dot = path.find_last_of(L'.');
  if (dot != std::wstring::npos) {
    std::wstring extension = path.substr(dot);
    std::transform(
        extension.begin(), extension.end(), extension.begin(), towlower);
    if (extension == L".png" || extension == L".jpg" ||
        extension == L".jpeg" || extension == L".webp" ||
        extension == L".gif" || extension == L".bmp") {
      return extension == L".jpeg" ? L".jpg" : extension;
    }
  }
  return L".img";
}

inline std::wstring CacheArtworkUrl(const fs::path& dataDir,
                                    const std::wstring& artworkUrl,
                                    const wchar_t* userAgent =
                                        L"HomePanel-Artwork-Cache/1.0") {
  if (artworkUrl.empty()) return {};
  if (artworkUrl.rfind(L"https://data.homepanel/", 0) == 0) return artworkUrl;

  const std::string key = WideToUtf8(artworkUrl);
  if (key.empty()) return artworkUrl;

  const std::wstring stem = Hex64(Fnv1a64(key));
  const fs::path cacheDir = dataDir / L"spotify-artwork-cache";
  std::error_code error;
  fs::create_directories(cacheDir, error);

  for (const wchar_t* extension :
       {L".jpg", L".png", L".webp", L".gif", L".bmp", L".img"}) {
    const fs::path cached = cacheDir / (stem + extension);
    if (fs::is_regular_file(cached, error) &&
        fs::file_size(cached, error) > 0) {
      return L"https://data.homepanel/spotify-artwork-cache/" +
          cached.filename().wstring();
    }
    error.clear();
  }

  std::vector<uint8_t> bytes;
  std::wstring contentType;
  if (!WinHttpDownload(artworkUrl.c_str(), 8 * 1024 * 1024, &bytes, &contentType,
                        nullptr, userAgent)) {
    return artworkUrl;
  }

  const fs::path cached =
      cacheDir / (stem + GuessArtworkExtension(contentType, artworkUrl));
  if (!AtomicWriteBytes(cached, bytes)) return artworkUrl;
  return L"https://data.homepanel/spotify-artwork-cache/" +
      cached.filename().wstring();
}
}
