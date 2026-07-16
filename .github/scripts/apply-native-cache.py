from pathlib import Path
import re


def replace_once(path: str, old: str, new: str) -> None:
    target = Path(path)
    text = target.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, found {count}: {old[:100]!r}")
    target.write_text(text.replace(old, new), encoding="utf-8", newline="")


def replace_regex(path: str, pattern: str, replacement: str) -> None:
    target = Path(path)
    text = target.read_text(encoding="utf-8")
    updated, count = re.subn(pattern, lambda _: replacement, text, count=1, flags=re.S)
    if count != 1:
        raise SystemExit(f"{path}: regex expected one match, found {count}: {pattern[:100]!r}")
    target.write_text(updated, encoding="utf-8", newline="")


replace_once(
    "native/src/web_renderer.h",
    r'''  struct NativeBackBuffer {
    HBITMAP bitmap = nullptr;
    int width = 0;
    int height = 0;
  };

  struct NativePanelPaintScope {''',
    r'''  struct NativeBackBuffer {
    HBITMAP bitmap = nullptr;
    int width = 0;
    int height = 0;
  };
  struct NativeSectionBitmapCache {
    HBITMAP bitmap = nullptr;
    int width = 0;
    int height = 0;
    uint64_t revision = 0;
    uint64_t layoutRevision = 0;
  };
  struct NativeAirGraphProjection {
    std::vector<AirHistorySample> samples;
    int64_t cutoff = 0;
    double co2Min = 0;
    double co2Max = 0;
    double temperatureMin = 0;
    double temperatureMax = 0;
    double humidityMin = 0;
    double humidityMax = 0;
  };

  struct NativePanelPaintScope {''')

replace_once(
    "native/src/web_renderer.h",
    r'''  void DrawEnergySection(HDC dc, const RECT& card);
  void DrawNewsSection(HDC dc, const RECT& card);
  HBITMAP NativePanelBackBuffer(HWND hwnd, HDC dc, int width, int height);''',
    r'''  void DrawEnergySection(HDC dc, const RECT& card);
  void DrawNewsSection(HDC dc, const RECT& card);
  void RebuildNativeAirGraph(int64_t nowMs);
  void DrawCachedPanelSection(HDC dc, const RECT& card, PanelSection section,
                              uint64_t revision,
                              void (Renderer::*draw)(HDC, const RECT&));
  HBITMAP NativePanelBackBuffer(HWND hwnd, HDC dc, int width, int height);''')

replace_once(
    "native/src/web_renderer.h",
    r'''  HBITMAP NativeWeatherIconBitmap(const std::wstring& icon, bool night, int width, int height);
  HBITMAP CacheNativeBitmap(const std::wstring& key, HBITMAP bitmap);
  void StartRadarCompose();''',
    r'''  HBITMAP NativeWeatherIconBitmap(const std::wstring& icon, bool night, int width, int height);
  HBITMAP CacheNativeBitmap(const std::wstring& key, HBITMAP bitmap);
  HBITMAP CachedRadarBitmap(const std::wstring& key, const fs::path& path,
                            int width, int height);
  void StartRadarCompose();''')

replace_once(
    "native/src/web_renderer.h",
    r'''  SensorSnapshot nativeSensors_{};
  std::vector<AirHistorySample> nativeAirHistory_;
  std::vector<StationheadPlayHistorySample> nativeStationheadPlayHistory_;''',
    r'''  SensorSnapshot nativeSensors_{};
  std::vector<AirHistorySample> nativeAirHistory_;
  NativeAirGraphProjection nativeAirGraph_{};
  std::vector<StationheadPlayHistorySample> nativeStationheadPlayHistory_;''')

replace_once(
    "native/src/web_renderer.h",
    r'''  int nativeNewsIndex_ = 0;
  uint64_t nativeRenderedDashboardRevision_ = 0;
  int width_ = 0;''',
    r'''  int nativeNewsIndex_ = 0;
  uint64_t nativeRenderedDashboardRevision_ = 0;
  uint64_t nativeRenderedWeatherRevision_ = 0;
  uint64_t nativeRenderedEnergyRevision_ = 0;
  uint64_t nativeRenderedNewsRevision_ = 0;
  uint64_t nativeAirRenderRevision_ = 0;
  uint64_t nativeNewsRenderRevision_ = 0;
  uint64_t nativeLayoutRevision_ = 1;
  int width_ = 0;''')

replace_once(
    "native/src/web_renderer.h",
    r'''  std::string dashboardUtf8_;
  uint64_t dashboardSourceRevision_ = 0;
  uint64_t spotifySourceRevision_ = 0;''',
    r'''  std::string dashboardUtf8_;
  fs::path dashboardSourcePath_;
  std::uintmax_t dashboardSourceSize_ = 0;
  fs::file_time_type dashboardSourceModifiedAt_{};
  bool dashboardSourceStampValid_ = false;
  uint64_t dashboardSourceRevision_ = 0;
  uint64_t dashboardWeatherRevision_ = 0;
  uint64_t dashboardEnergyRevision_ = 0;
  uint64_t dashboardNewsRevision_ = 0;
  uint64_t spotifySourceRevision_ = 0;''')

replace_once(
    "native/src/web_renderer.h",
    r'''  std::map<std::wstring, ArtworkBitmapCacheEntry> nativeArtworkBitmaps_;
  uint64_t nativeArtworkUseCounter_ = 0;
  std::map<HWND, NativeBackBuffer> nativeBackBuffers_;''',
    r'''  std::map<std::wstring, ArtworkBitmapCacheEntry> nativeArtworkBitmaps_;
  uint64_t nativeArtworkUseCounter_ = 0;
  std::map<HWND, NativeBackBuffer> nativeBackBuffers_;
  std::map<PanelSection, NativeSectionBitmapCache> nativeSectionBitmaps_;''')

replace_once(
    "native/src/web_renderer.h",
    r'''  std::wstring radarSignature_;
  std::map<std::wstring, int64_t> radarFailedTiles_;
  bool radarComposePending_ = false;''',
    r'''  std::wstring radarSignature_;
  std::map<std::wstring, int64_t> radarFailedTiles_;
  std::map<std::wstring, ArtworkBitmapCacheEntry> radarBitmaps_;
  uint64_t radarBitmapUseCounter_ = 0;
  bool radarComposePending_ = false;''')

replace_once(
    "native/src/dashboard_data.h",
    r'''  bool switchBotDoorOpen = false;
  bool switchBotMotion = false;
  std::vector<SwitchBotDeviceData> switchBotDevices;
};''',
    r'''  bool switchBotDoorOpen = false;
  bool switchBotMotion = false;
  std::vector<SwitchBotDeviceData> switchBotDevices;

  uint64_t weatherRevision = 0;
  uint64_t newsRevision = 0;
  uint64_t octopusRevision = 0;
  uint64_t switchBotRevision = 0;
};''')

replace_once(
    "native/src/dashboard_data.cpp",
    r'''using winrt::Windows::Data::Json::JsonValueType;

double NumberOrNaN''',
    r'''using winrt::Windows::Data::Json::JsonValueType;

uint64_t SectionRevision(const JsonObject& object, const std::wstring& cloudError) {
  std::string source = WideToUtf8(std::wstring(object.Stringify().c_str()));
  source.push_back('\0');
  source += WideToUtf8(cloudError);
  return Fnv1a64(source);
}

double NumberOrNaN''')

for old, new in (
    (
        '    const JsonObject weather = json::Object(root, L"weather");\n    next.weatherStatus = ReadStatus(weather);',
        '    const JsonObject weather = json::Object(root, L"weather");\n    next.weatherRevision = SectionRevision(weather, next.cloudError);\n    next.weatherStatus = ReadStatus(weather);',
    ),
    (
        '    const JsonObject news = json::Object(root, L"news");\n    next.newsStatus = ReadStatus(news);',
        '    const JsonObject news = json::Object(root, L"news");\n    next.newsRevision = SectionRevision(news, next.cloudError);\n    next.newsStatus = ReadStatus(news);',
    ),
    (
        '    const JsonObject octopus = json::Object(root, L"octopus");\n    next.octopusStatus = ReadStatus(octopus);',
        '    const JsonObject octopus = json::Object(root, L"octopus");\n    next.octopusRevision = SectionRevision(octopus, next.cloudError);\n    next.octopusStatus = ReadStatus(octopus);',
    ),
    (
        '    const JsonObject switchbot = json::Object(root, L"switchbot");\n    next.switchBotStatus = ReadStatus(switchbot);',
        '    const JsonObject switchbot = json::Object(root, L"switchbot");\n    next.switchBotRevision = SectionRevision(switchbot, next.cloudError);\n    next.switchBotStatus = ReadStatus(switchbot);',
    ),
):
    replace_once("native/src/dashboard_data.cpp", old, new)

replace_once(
    "native/src/renderer_state.cpp",
    '  width_ = nextWidth;\n  height_ = nextHeight;',
    '  width_ = nextWidth;\n  height_ = nextHeight;\n  ++nativeLayoutRevision_;')
replace_once(
    "native/src/renderer_state.cpp",
    '  bounds_ = bounds;\n  width_ = std::max(1L, bounds.right - bounds.left);',
    '  bounds_ = bounds;\n  ++nativeLayoutRevision_;\n  width_ = std::max(1L, bounds.right - bounds.left);')

replace_regex(
    "native/src/renderer_state.cpp",
    r'''bool Renderer::LoadDashboard\(const fs::path& jsonPath, bool\* changed\) \{.*?\n\}\n\nRECT Renderer::ClientBounds''',
    r'''bool Renderer::LoadDashboard(const fs::path& jsonPath, bool* changed) {
  if (changed) *changed = false;
  try {
    std::error_code error;
    const fs::path normalizedPath = jsonPath.lexically_normal();
    const std::uintmax_t sourceSize = fs::file_size(normalizedPath, error);
    if (error) return false;
    const fs::file_time_type modifiedAt = fs::last_write_time(normalizedPath, error);
    if (error) return false;
    if (dashboardSourceStampValid_ && dashboardSourcePath_ == normalizedPath &&
        dashboardSourceSize_ == sourceSize && dashboardSourceModifiedAt_ == modifiedAt) {
      return true;
    }

    std::ifstream input(normalizedPath, std::ios::binary);
    if (!input) return false;
    std::string text((std::istreambuf_iterator<char>(input)), {});
    if (text.empty()) return false;
    if (text == dashboardUtf8_) {
      dashboardSourcePath_ = normalizedPath;
      dashboardSourceSize_ = sourceSize;
      dashboardSourceModifiedAt_ = modifiedAt;
      dashboardSourceStampValid_ = true;
      return true;
    }

    DashboardSnapshot snapshot;
    if (!ParseDashboardSnapshot(text, snapshot)) return false;
    const bool firstSnapshot = !nativeDashboard_.loaded;
    const bool weatherChanged = firstSnapshot ||
        snapshot.weatherRevision != nativeDashboard_.weatherRevision;
    const bool newsChanged = firstSnapshot ||
        snapshot.newsRevision != nativeDashboard_.newsRevision;
    const bool energyChanged = firstSnapshot ||
        snapshot.octopusRevision != nativeDashboard_.octopusRevision ||
        snapshot.switchBotRevision != nativeDashboard_.switchBotRevision;
    const bool contentChanged = weatherChanged || newsChanged || energyChanged;

    newsCount_ = snapshot.newsItemCount;
    nativeDashboard_ = std::move(snapshot);
    dashboardUtf8_ = std::move(text);
    dashboardSourcePath_ = normalizedPath;
    dashboardSourceSize_ = sourceSize;
    dashboardSourceModifiedAt_ = modifiedAt;
    dashboardSourceStampValid_ = true;
    if (weatherChanged) ++dashboardWeatherRevision_;
    if (newsChanged) ++dashboardNewsRevision_;
    if (energyChanged) ++dashboardEnergyRevision_;
    if (contentChanged) ++dashboardSourceRevision_;
    if (changed) *changed = contentChanged;
    return true;
  } catch (...) {
    return false;
  }
}

RECT Renderer::ClientBounds''')

replace_regex(
    "native/src/renderer_panels/part2.inc",
    r'''(HFONT Renderer::TierFont\(FontTier tier\) const \{.*?\n\})\n\nRenderer::NativePanelPaintScope::NativePanelPaintScope''',
    r'''\1

void Renderer::RebuildNativeAirGraph(int64_t nowMs) {
  constexpr int64_t kWindowMs = 24LL * 60 * 60 * 1000;
  NativeAirGraphProjection next;
  next.cutoff = nowMs - kWindowMs;
  next.samples.reserve(nativeAirHistory_.size());
  double co2Min = std::numeric_limits<double>::max();
  double co2Max = std::numeric_limits<double>::lowest();
  double temperatureMin = std::numeric_limits<double>::max();
  double temperatureMax = std::numeric_limits<double>::lowest();
  double humidityMin = std::numeric_limits<double>::max();
  double humidityMax = std::numeric_limits<double>::lowest();
  for (const auto& sample : nativeAirHistory_) {
    if (sample.timestamp < next.cutoff || sample.co2 < 250 || sample.co2 > 10000 ||
        sample.temperature < -40 || sample.temperature > 85 ||
        sample.humidity < 0 || sample.humidity > 100) {
      continue;
    }
    next.samples.push_back(sample);
    co2Min = std::min(co2Min, static_cast<double>(sample.co2));
    co2Max = std::max(co2Max, static_cast<double>(sample.co2));
    temperatureMin = std::min(temperatureMin, sample.temperature);
    temperatureMax = std::max(temperatureMax, sample.temperature);
    humidityMin = std::min(humidityMin, sample.humidity);
    humidityMax = std::max(humidityMax, sample.humidity);
  }
  if (!next.samples.empty()) {
    next.co2Min = co2Min;
    next.co2Max = co2Max;
    next.temperatureMin = temperatureMin;
    next.temperatureMax = temperatureMax;
    next.humidityMin = humidityMin;
    next.humidityMax = humidityMax;
  }
  nativeAirGraph_ = std::move(next);
}

void Renderer::DrawCachedPanelSection(
    HDC dc, const RECT& card, PanelSection section, uint64_t revision,
    void (Renderer::*draw)(HDC, const RECT&)) {
  const int width = std::max(1, static_cast<int>(card.right - card.left));
  const int height = std::max(1, static_cast<int>(card.bottom - card.top));
  NativeSectionBitmapCache& cache = nativeSectionBitmaps_[section];
  const bool stale = !cache.bitmap || cache.width != width || cache.height != height ||
      cache.revision != revision || cache.layoutRevision != nativeLayoutRevision_;
  if (stale) {
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    HDC memoryDc = bitmap ? CreateCompatibleDC(dc) : nullptr;
    if (bitmap && memoryDc) {
      HGDIOBJ previous = SelectObject(memoryDc, bitmap);
      const RECT local{0, 0, width, height};
      FillWidgetBackground(memoryDc, local);
      SetBkMode(memoryDc, TRANSPARENT);
      (this->*draw)(memoryDc, local);
      SelectObject(memoryDc, previous);
      DeleteDC(memoryDc);
      if (cache.bitmap) DeleteObject(cache.bitmap);
      cache = NativeSectionBitmapCache{bitmap, width, height, revision, nativeLayoutRevision_};
    } else {
      if (memoryDc) DeleteDC(memoryDc);
      if (bitmap) DeleteObject(bitmap);
    }
  }
  if (!cache.bitmap) {
    (this->*draw)(dc, card);
    return;
  }
  HDC sourceDc = CreateCompatibleDC(dc);
  if (!sourceDc) {
    (this->*draw)(dc, card);
    return;
  }
  HGDIOBJ previous = SelectObject(sourceDc, cache.bitmap);
  BitBlt(dc, card.left, card.top, width, height, sourceDc, 0, 0, SRCCOPY);
  SelectObject(sourceDc, previous);
  DeleteDC(sourceDc);
}

Renderer::NativePanelPaintScope::NativePanelPaintScope''')

replace_once(
    "native/src/renderer_panels/part2.inc",
    '  nativeBackBuffers_.clear();\n  for (auto& [key, entry] : nativeArtworkBitmaps_) {',
    '''  nativeBackBuffers_.clear();
  for (auto& [section, cache] : nativeSectionBitmaps_) {
    if (cache.bitmap) DeleteObject(cache.bitmap);
  }
  nativeSectionBitmaps_.clear();
  for (auto& [key, entry] : nativeArtworkBitmaps_) {''')
replace_once(
    "native/src/renderer_panels/part2.inc",
    '  nativeArtworkBitmaps_.clear();\n  nativeArtworkUseCounter_ = 0;\n  {',
    '''  nativeArtworkBitmaps_.clear();
  nativeArtworkUseCounter_ = 0;
  for (auto& [key, entry] : radarBitmaps_) {
    if (entry.bitmap) DeleteObject(entry.bitmap);
  }
  radarBitmaps_.clear();
  radarBitmapUseCounter_ = 0;
  {''')

replace_regex(
    "native/src/renderer_panels/part2.inc",
    r'''void Renderer::UpdateNativeStaticPanels\(const RenderState& state\) \{.*?\n\}\n\nvoid Renderer::TickNativePanels''',
    r'''void Renderer::UpdateNativeStaticPanels(const RenderState& state) {
  const bool sensorsChanged = nativeSensors_ != state.sensors;
  const bool historyChanged = nativeAirHistory_ != state.airHistory;
  const bool stationheadChanged = nativeStationhead_ != state.stationhead;
  const bool stationheadHistoryChanged = nativeStationheadPlayHistory_ != state.stationheadPlayHistory;
  const bool controlsChanged = nativeAppVersion_ != state.appVersion || nativeToast_ != state.toast;
  const bool newsIndexChanged = nativeNewsIndex_ != state.newsIndex;
  const bool weatherChanged = nativeRenderedWeatherRevision_ != dashboardWeatherRevision_;
  const bool energyChanged = nativeRenderedEnergyRevision_ != dashboardEnergyRevision_;
  const bool newsChanged = nativeRenderedNewsRevision_ != dashboardNewsRevision_;

  if (sensorsChanged) nativeSensors_ = state.sensors;
  if (historyChanged) {
    nativeAirHistory_ = state.airHistory;
    RebuildNativeAirGraph(UnixMillis());
  }
  if (sensorsChanged || historyChanged) ++nativeAirRenderRevision_;
  if (stationheadHistoryChanged) nativeStationheadPlayHistory_ = state.stationheadPlayHistory;
  if (stationheadChanged) nativeStationhead_ = state.stationhead;
  if (controlsChanged) {
    nativeAppVersion_ = state.appVersion;
    nativeToast_ = state.toast;
  }
  if (newsIndexChanged) nativeNewsIndex_ = state.newsIndex;
  if (weatherChanged) nativeRenderedWeatherRevision_ = dashboardWeatherRevision_;
  if (energyChanged) nativeRenderedEnergyRevision_ = dashboardEnergyRevision_;
  if (newsChanged) nativeRenderedNewsRevision_ = dashboardNewsRevision_;
  if (newsChanged || newsIndexChanged) ++nativeNewsRenderRevision_;
  nativeRenderedDashboardRevision_ = dashboardSourceRevision_;
  if (!EnsureNativeStaticWindows()) return;
  if (sensorsChanged || historyChanged) {
    InvalidatePanelSection(nativeMainWindow_, PanelSection::Energy);
  }
  if (weatherChanged) InvalidatePanelSection(nativeSideWindow_, PanelSection::Air);
  if (energyChanged) InvalidatePanelSection(nativeSideWindow_, PanelSection::Weather);
  if (newsChanged || newsIndexChanged) {
    InvalidatePanelSection(nativeMainWindow_, PanelSection::News);
  }
  if (controlsChanged) InvalidatePanelSection(nativeSideWindow_, PanelSection::Controls);
  if (stationheadChanged || stationheadHistoryChanged) {
    InvalidatePanelSection(nativeMainWindow_, PanelSection::Music);
  }
}

void Renderer::TickNativePanels''')

replace_once(
    "native/src/renderer_panels/part2.inc",
    '''  if (IntersectRect(&overlap, &scope.dirty, &sections.air)) {
    DrawWeatherSection(scope.dc, sections.air);
  }
  if (IntersectRect(&overlap, &scope.dirty, &sections.weather)) {
    DrawEnergySection(scope.dc, sections.weather);
  }''',
    '''  if (IntersectRect(&overlap, &scope.dirty, &sections.air)) {
    DrawCachedPanelSection(scope.dc, sections.air, PanelSection::Weather,
                           dashboardWeatherRevision_, &Renderer::DrawWeatherSection);
  }
  if (IntersectRect(&overlap, &scope.dirty, &sections.weather)) {
    DrawCachedPanelSection(scope.dc, sections.weather, PanelSection::Energy,
                           dashboardEnergyRevision_, &Renderer::DrawEnergySection);
  }''')
replace_once(
    "native/src/renderer_panels/part2.inc",
    '''  if (IntersectRect(&overlap, &scope.dirty, &sections.energy)) {
    DrawAirSection(scope.dc, sections.energy);
  }
  if (IntersectRect(&overlap, &scope.dirty, &sections.news)) {
    DrawNewsSection(scope.dc, sections.news);
  }''',
    '''  if (IntersectRect(&overlap, &scope.dirty, &sections.energy)) {
    DrawCachedPanelSection(scope.dc, sections.energy, PanelSection::Air,
                           nativeAirRenderRevision_, &Renderer::DrawAirSection);
  }
  if (IntersectRect(&overlap, &scope.dirty, &sections.news)) {
    DrawCachedPanelSection(scope.dc, sections.news, PanelSection::News,
                           nativeNewsRenderRevision_, &Renderer::DrawNewsSection);
  }''')

replace_regex(
    "native/src/renderer_panels/part3.inc",
    r'''  const int64_t now = UnixMillis\(\);.*?\n  const int plotTop =''',
    r'''  constexpr int64_t kWindowMs = 24LL * 60 * 60 * 1000;
  const auto& samples = nativeAirGraph_.samples;
  const int64_t cutoff = nativeAirGraph_.cutoff;

  const int plotTop =''')
replace_once(
    "native/src/renderer_panels/part3.inc",
    '''    DrawHistoryLine(dc, samples, plot, cutoff, kWindowMs, co2Min - 80, co2Max + 80, kWidgetGreen, 2,
                    [](const AirHistorySample& s) { return static_cast<double>(s.co2); });
    DrawHistoryLine(dc, samples, plot, cutoff, kWindowMs, temperatureMin - 0.5, temperatureMax + 0.5,
                    kWidgetOrange, 1,
                    [](const AirHistorySample& s) { return s.temperature; });
    DrawHistoryLine(dc, samples, plot, cutoff, kWindowMs, humidityMin - 2, humidityMax + 2,
                    kWidgetBlueMuted, 1,
                    [](const AirHistorySample& s) { return s.humidity; });''',
    '''    DrawHistoryLine(dc, samples, plot, cutoff, kWindowMs,
                    nativeAirGraph_.co2Min - 80, nativeAirGraph_.co2Max + 80,
                    kWidgetGreen, 2,
                    [](const AirHistorySample& s) { return static_cast<double>(s.co2); });
    DrawHistoryLine(dc, samples, plot, cutoff, kWindowMs,
                    nativeAirGraph_.temperatureMin - 0.5,
                    nativeAirGraph_.temperatureMax + 0.5,
                    kWidgetOrange, 1,
                    [](const AirHistorySample& s) { return s.temperature; });
    DrawHistoryLine(dc, samples, plot, cutoff, kWindowMs,
                    nativeAirGraph_.humidityMin - 2,
                    nativeAirGraph_.humidityMax + 2,
                    kWidgetBlueMuted, 1,
                    [](const AirHistorySample& s) { return s.humidity; });''')

replace_once(
    "native/src/renderer_radar_ui.cpp",
    "constexpr int64_t kRadarTileFailureTtlMs = 5 * 60'000;",
    "constexpr int64_t kRadarTileFailureTtlMs = 5 * 60'000;\nconstexpr size_t kRadarBitmapCacheLimit = 16;")
replace_regex(
    "native/src/renderer_radar_ui.cpp",
    r'''HBITMAP DecodeRadarTile\(.*?\n\}\n\nbool FileMatchesText''',
    r'''std::optional<fs::path> RadarTilePath(const fs::path& dataDir,
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

bool FileMatchesText''')
replace_once(
    "native/src/renderer_radar_ui.cpp",
    "}  // namespace\n\nvoid Renderer::StartRadarCompose() {",
    r'''}  // namespace

HBITMAP Renderer::CachedRadarBitmap(const std::wstring& key, const fs::path& path,
                                    int width, int height) {
  if (width <= 0 || height <= 0) return nullptr;
  const std::wstring cacheKey = key + L"#" + Utf8ToWide(FileStamp(path)) + L"#" +
      std::to_wstring(width) + L"x" + std::to_wstring(height);
  auto found = radarBitmaps_.find(cacheKey);
  if (found != radarBitmaps_.end()) {
    found->second.lastUsed = ++radarBitmapUseCounter_;
    return found->second.bitmap;
  }
  HBITMAP bitmap = DecodeImageFileToBitmap(path, width, height);
  if (!bitmap) return nullptr;
  if (radarBitmaps_.size() >= kRadarBitmapCacheLimit) {
    auto oldest = radarBitmaps_.begin();
    for (auto item = radarBitmaps_.begin(); item != radarBitmaps_.end(); ++item) {
      if (item->second.lastUsed < oldest->second.lastUsed) oldest = item;
    }
    if (oldest->second.bitmap) DeleteObject(oldest->second.bitmap);
    radarBitmaps_.erase(oldest);
  }
  radarBitmaps_[cacheKey] = ArtworkBitmapCacheEntry{bitmap, ++radarBitmapUseCounter_};
  return bitmap;
}

void Renderer::StartRadarCompose() {''')
replace_once(
    "native/src/renderer_radar_ui.cpp",
    '''  HBITMAP radarSatelliteBitmap = DecodeImageFileToBitmap(
      satellitePath, kRadarCanvasWidth, kRadarCanvasHeight);
  HBITMAP radarMapBitmap = DecodeImageFileToBitmap(
      mapPath, kRadarCanvasWidth, kRadarCanvasHeight);
  if (!radarSatelliteBitmap || !radarMapBitmap) {
    if (radarSatelliteBitmap) DeleteObject(radarSatelliteBitmap);
    if (radarMapBitmap) DeleteObject(radarMapBitmap);
    return;
  }''',
    '''  HBITMAP radarSatelliteBitmap = CachedRadarBitmap(
      L"radar-satellite", satellitePath, kRadarCanvasWidth, kRadarCanvasHeight);
  HBITMAP radarMapBitmap = CachedRadarBitmap(
      L"radar-map", mapPath, kRadarCanvasWidth, kRadarCanvasHeight);
  if (!radarSatelliteBitmap || !radarMapBitmap) return;''')
replace_once(
    "native/src/renderer_radar_ui.cpp",
    '''  if (!composed) {
    DeleteObject(radarSatelliteBitmap);
    DeleteObject(radarMapBitmap);
    return;
  }''',
    '''  if (!composed) return;''')
replace_once(
    "native/src/renderer_radar_ui.cpp",
    '''  if (!composeDc) {
    DeleteObject(composed);
    DeleteObject(radarSatelliteBitmap);
    DeleteObject(radarMapBitmap);
    return;
  }''',
    '''  if (!composeDc) {
    DeleteObject(composed);
    return;
  }''')
replace_once(
    "native/src/renderer_radar_ui.cpp",
    '''    HBITMAP tileBitmap = DecodeRadarTile(dataDir_, tile.url, tileWidth, tileHeight);
    if (!tileBitmap) {
      radarFailedTiles_[tile.url] = now + kRadarTileFailureTtlMs;
      continue;
    }
    BlendBitmap(composeDc, tileBitmap,
                static_cast<int>(std::lround(tile.destination.x * scaleX)),
                static_cast<int>(std::lround(tile.destination.y * scaleY)),
                tileWidth, tileHeight);
    DeleteObject(tileBitmap);
    ++loadedTiles;''',
    '''    const std::optional<fs::path> tilePath = RadarTilePath(dataDir_, tile.url);
    HBITMAP tileBitmap = tilePath
        ? CachedRadarBitmap(L"radar-tile:" + tile.url, *tilePath, tileWidth, tileHeight)
        : nullptr;
    if (!tileBitmap) {
      radarFailedTiles_[tile.url] = now + kRadarTileFailureTtlMs;
      continue;
    }
    BlendBitmap(composeDc, tileBitmap,
                static_cast<int>(std::lround(tile.destination.x * scaleX)),
                static_cast<int>(std::lround(tile.destination.y * scaleY)),
                tileWidth, tileHeight);
    ++loadedTiles;''')
replace_once(
    "native/src/renderer_radar_ui.cpp",
    '''  SelectObject(composeDc, previousComposed);
  DeleteDC(composeDc);
  DeleteObject(radarSatelliteBitmap);
  DeleteObject(radarMapBitmap);
''',
    '''  SelectObject(composeDc, previousComposed);
  DeleteDC(composeDc);
''')
