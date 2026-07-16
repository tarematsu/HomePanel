#include "common.h"
#include "file_utils.h"
#include "version.h"

namespace hp {
namespace {
std::string ReadResource(int id) {
  HMODULE module = GetModuleHandleW(nullptr);
  HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
  if (!resource) return {};
  HGLOBAL loaded = ::LoadResource(module, resource);
  const char* bytes = loaded ? static_cast<const char*>(LockResource(loaded)) : nullptr;
  const DWORD size = SizeofResource(module, resource);
  if (!bytes || !size) return {};
  return std::string(bytes, bytes + size);
}


bool WriteContent(const fs::path& path, const std::string& content) {
  if (content.empty()) return false;
  if (file::MatchesText(path, content)) return true;
  return AtomicWriteBytes(path, content.data(), static_cast<DWORD>(content.size()));
}

struct RuntimeAsset {
  int id;
  const wchar_t* name;
};

#include "weather_icon_assets.h"

constexpr RuntimeAsset kRuntimeAssets[] = {
    {110, L"radar-satellite.png"},
    {112, L"radar-map.png"},
};


std::string ExecutableStamp(const fs::path& executable) {
  std::error_code error;
  const auto size = fs::file_size(executable, error);
  std::ostringstream stamp;
  stamp << WideToUtf8(kVersion) << "|native-assets-v3";
  if (!error) stamp << '|' << size;
  for (const RuntimeAsset& asset : kRuntimeAssets) {
    const std::string content = ReadResource(asset.id);
    stamp << '|' << asset.id << ':' << content.size() << ':' << std::hex << Fnv1a64(content) << std::dec;
  }
  for (const RuntimeAsset& asset : kWeatherIconAssets) {
    const std::string content = ReadResource(asset.id);
    stamp << '|' << asset.id << ':' << content.size() << ':' << std::hex << Fnv1a64(content) << std::dec;
  }
  return stamp.str();
}

bool RuntimeAssetsReady(const fs::path& folder, const std::string& stamp) {
  if (!file::MatchesText(folder / L"native-assets.signature", stamp)) return false;
  for (const RuntimeAsset& asset : kRuntimeAssets) {
    if (!file::NonEmpty(folder / asset.name)) return false;
  }
  for (const RuntimeAsset& asset : kWeatherIconAssets) {
    if (!file::NonEmpty(folder / asset.name)) return false;
  }
  return true;
}

void RemoveObsoleteDashboardFiles(const fs::path& folder) {
  static constexpr const wchar_t* kFiles[] = {
      L"index.html",
      L"styles.css",
      L"app.js",
      L"texts.json",
      L"air-history.css",
      L"air-history.js",
      L"homepanel-core.js",
      L"homepanel-clock.js",
      L"homepanel-news.js",
      L"homepanel-weather.js",
      L"homepanel-energy.js",
      L"homepanel-switchbot.js",
      L"homepanel-air.js",
      L"homepanel-radar.js",
      L"homepanel-runtime.js",
      L"spotify-panel-runtime.js",
      L"playback-shared.js",
      L"wallpaper.css",
      L"wallpaper-homepanel.png",
      L"wallpaper-homepanel.signature",
      L"ui-bundle.signature",
      L"ui-runtime-final.signature",
      L"ui-styles.signature",
      L"stationhead-audio-controls.js",
      L"stationhead-playback.js",
      L"radar-direct.js",
      L"radar-base.png",
      L"performance.css",
      L"ui-overrides.css",
      L"canvas-transparency.js",
      L"runtime-performance.js",
      L"radar-monochrome.js",
  };
  std::error_code error;
  for (const wchar_t* name : kFiles) {
    fs::remove(folder / name, error);
    error.clear();
  }
  fs::remove_all(folder / L"vendor", error);
}
}

bool InstallRuntimeAssets() noexcept {
  try {
    wchar_t executableRaw[MAX_PATH * 4]{};
    if (!GetModuleFileNameW(nullptr, executableRaw, _countof(executableRaw))) return false;
    const fs::path executable(executableRaw);
    const fs::path folder = executable.parent_path() / L"ui";
    std::error_code error;
    fs::create_directories(folder, error);
    if (error) return false;

    const std::string signature = ExecutableStamp(executable);
    if (!RuntimeAssetsReady(folder, signature)) {
      bool installed = true;
      for (const RuntimeAsset& asset : kRuntimeAssets) {
        installed = WriteContent(folder / asset.name, ReadResource(asset.id)) && installed;
      }
      fs::create_directories(folder / L"weather-icons", error);
      if (error) installed = false;
      for (const RuntimeAsset& asset : kWeatherIconAssets) {
        installed = WriteContent(folder / asset.name, ReadResource(asset.id)) && installed;
      }
      if (!installed || !WriteContent(folder / L"native-assets.signature", signature)) return false;
    }
    RemoveObsoleteDashboardFiles(folder);
    return true;
  } catch (...) {
    return false;
  }
}

}
