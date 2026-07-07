#include "common.h"
#include "version.h"
#include "wallpaper_optimizer.h"

namespace {
std::string ReadResource(int id) {
  HMODULE module = GetModuleHandleW(nullptr);
  HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
  if (!resource) return {};
  HGLOBAL loaded = LoadResource(resource);
  const char* bytes = loaded ? static_cast<const char*>(LockResource(loaded)) : nullptr;
  const DWORD size = SizeofResource(module, resource);
  if (!bytes || !size) return {};
  return std::string(bytes, bytes + size);
}

std::string ReadTextResource(int id) {
  std::string content = ReadResource(id);
  if (content.size() >= 3 &&
      static_cast<unsigned char>(content[0]) == 0xef &&
      static_cast<unsigned char>(content[1]) == 0xbb &&
      static_cast<unsigned char>(content[2]) == 0xbf) {
    content.erase(0, 3);
  }
  return content;
}

bool FileMatches(const hp::fs::path& path, const std::string& content) {
  std::error_code error;
  if (!hp::fs::is_regular_file(path, error) || hp::fs::file_size(path, error) != content.size()) return false;
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  return std::equal(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>(), content.begin(), content.end());
}

bool NonEmptyFile(const hp::fs::path& path) {
  std::error_code error;
  return hp::fs::is_regular_file(path, error) && hp::fs::file_size(path, error) > 0;
}

bool WriteContent(const hp::fs::path& path, const std::string& content) {
  if (content.empty()) return false;
  if (FileMatches(path, content)) return true;
  return hp::AtomicWriteBytes(path, content.data(), static_cast<DWORD>(content.size()));
}

struct UiAsset {
  int id;
  const wchar_t* name;
  bool text;
};

constexpr UiAsset kUiAssets[] = {
    {101, L"index.html", true},
    {102, L"styles.css", true},
    {103, L"app.js", true},
    {104, L"texts.json", true},
    {105, L"air-history.css", true},
    {106, L"air-history.js", true},
    {107, L"performance.css", true},
    {109, L"ui-overrides.css", true},
    {110, L"radar-satellite.png", false},
    {112, L"radar-map.png", false},
    {113, L"canvas-transparency.js", true},
    {114, L"spotify-panel-runtime.js", true},
    {115, L"runtime-performance.js", true},
    {137, L"playback-shared.js", true},
    {117, L"homepanel-core.js", true},
    {118, L"homepanel-clock.js", true},
    {119, L"homepanel-news.js", true},
    {120, L"homepanel-weather.js", true},
    {121, L"homepanel-energy.js", true},
    {122, L"homepanel-switchbot.js", true},
    {123, L"homepanel-sh.js", true},
    {124, L"homepanel-air.js", true},
    {125, L"homepanel-radar.js", true},
    {126, L"homepanel-runtime.js", true},
    {127, L"radar-monochrome.js", true},
    {129, L"vendor/wx-icons/100.svg", true},
    {130, L"vendor/wx-icons/101.svg", true},
    {131, L"vendor/wx-icons/200.svg", true},
    {132, L"vendor/wx-icons/206.svg", true},
    {133, L"vendor/wx-icons/212.svg", true},
    {134, L"vendor/wx-icons/300.svg", true},
    {135, L"vendor/wx-icons/313.svg", true},
    {136, L"vendor/wx-icons/400.svg", true},
};

std::string AssetContent(const UiAsset& asset) {
  return asset.text ? ReadTextResource(asset.id) : ReadResource(asset.id);
}

bool WriteAsset(const hp::fs::path& folder, const UiAsset& asset) {
  return WriteContent(folder / asset.name, AssetContent(asset));
}

hp::fs::path CurrentWallpaper() {
  wchar_t path[MAX_PATH * 4]{};
  if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, _countof(path), path, 0) || !path[0]) return {};
  hp::fs::path wallpaper(path);
  std::error_code error;
  return hp::fs::is_regular_file(wallpaper, error) ? wallpaper : hp::fs::path{};
}

std::string ExecutableStamp(const hp::fs::path& executable) {
  std::error_code error;
  const auto size = hp::fs::file_size(executable, error);
  if (error) return hp::WideToUtf8(hp::kVersion) + "|ui-static-v1";
  const auto modified = hp::fs::last_write_time(executable, error);
  std::ostringstream stamp;
  stamp << hp::WideToUtf8(hp::kVersion) << "|ui-static-v1|" << size;
  if (!error) stamp << '|' << modified.time_since_epoch().count();
  return stamp.str();
}

std::string StyleStamp(const std::string& executableStamp) {
  std::ostringstream stamp;
  stamp << executableStamp << "|wallpaper-v1|" << GetSystemMetrics(SM_CXSCREEN)
        << 'x' << GetSystemMetrics(SM_CYSCREEN);
  const hp::fs::path source = CurrentWallpaper();
  if (source.empty()) return stamp.str() + "|none";
  std::error_code error;
  const auto size = hp::fs::file_size(source, error);
  const auto modified = hp::fs::last_write_time(source, error);
  stamp << '|' << hp::WideToUtf8(source.wstring());
  if (!error) stamp << '|' << size << '|' << modified.time_since_epoch().count();
  return stamp.str();
}

std::string WallpaperCss(const hp::fs::path& folder) {
  std::error_code error;
  std::string background =
      "html,body{background-color:#05080d;}"
      "body{background-position:center center;background-size:cover;"
      "background-repeat:no-repeat;}\n";

  hp::fs::path destination;
  const hp::fs::path source = CurrentWallpaper();
  if (!source.empty()) {
    destination = hp::PrepareDashboardWallpaper(source, folder);
    if (destination.empty()) {
      std::wstring extension = source.extension().wstring();
      std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
      if (extension == L".jpg" || extension == L".jpeg" || extension == L".png" ||
          extension == L".bmp" || extension == L".webp") {
        destination = folder / (L"wallpaper-fallback" + extension);
        hp::fs::copy_file(source, destination, hp::fs::copy_options::overwrite_existing, error);
        if (error) destination.clear();
      }
    }
    error.clear();
    if (!destination.empty() && hp::fs::is_regular_file(destination, error)) {
      background += "body{background-image:url('" + hp::WideToUtf8(destination.filename().wstring()) + "');}\n";
    }
  }

  error.clear();
  for (const auto& item : hp::fs::directory_iterator(folder, error)) {
    if (error) break;
    const std::wstring name = item.path().filename().wstring();
    const bool wallpaperFile = name.rfind(L"wallpaper", 0) == 0;
    const bool signature = name == L"wallpaper-homepanel.signature";
    if (item.is_regular_file() && wallpaperFile && !signature && item.path() != destination) {
      hp::fs::remove(item.path(), error);
      error.clear();
    }
  }
  return background;
}

bool StaticUiReady(const hp::fs::path& folder, const std::string& stamp) {
  if (!FileMatches(folder / L"ui-bundle.signature", stamp)) return false;
  for (const UiAsset& asset : kUiAssets) {
    if (!NonEmptyFile(folder / asset.name)) return false;
  }
  return true;
}

void RemoveObsoleteGeneratedFiles(const hp::fs::path& folder) {
  static constexpr const wchar_t* kFiles[] = {
      L"ui-runtime-final.signature",
      L"ui-styles.signature",
      L"stationhead-audio-controls.js",
      L"stationhead-playback.js",
      L"radar-direct.js",
      L"radar-base.png",
  };
  std::error_code error;
  for (const wchar_t* name : kFiles) {
    hp::fs::remove(folder / name, error);
    error.clear();
  }
  hp::fs::remove(folder / L"vendor", error);
}

struct EmbeddedUiInstaller {
  EmbeddedUiInstaller() {
    wchar_t executableRaw[MAX_PATH * 4]{};
    if (!GetModuleFileNameW(nullptr, executableRaw, _countof(executableRaw))) return;
    const hp::fs::path executable(executableRaw);
    const hp::fs::path folder = executable.parent_path() / L"ui";
    std::error_code error;
    hp::fs::create_directories(folder, error);

    const std::string executableSignature = ExecutableStamp(executable);
    if (!StaticUiReady(folder, executableSignature)) {
      bool installed = true;
      for (const UiAsset& asset : kUiAssets) {
        installed = WriteAsset(folder, asset) && installed;
      }
      if (installed) WriteContent(folder / L"ui-bundle.signature", executableSignature);
      RemoveObsoleteGeneratedFiles(folder);
    }

    const std::string styleSignature = StyleStamp(executableSignature);
    if (!FileMatches(folder / L"wallpaper-homepanel.signature", styleSignature) ||
        !NonEmptyFile(folder / L"wallpaper.css")) {
      if (WriteContent(folder / L"wallpaper.css", WallpaperCss(folder))) {
        WriteContent(folder / L"wallpaper-homepanel.signature", styleSignature);
      }
    }
  }
};
EmbeddedUiInstaller installer;
}  // namespace
