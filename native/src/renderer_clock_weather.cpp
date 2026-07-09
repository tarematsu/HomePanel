#include "web_renderer.h"
#include "wic_image.h"

namespace hp {
namespace {
constexpr int kNativeAirId = 101;
constexpr int kNativeAirHistoryId = 102;
constexpr int kNativeControlsId = 103;
constexpr int kNativeNewsId = 104;
constexpr int kNativeWeatherId = 105;
constexpr int kNativeEnergyId = 106;
constexpr int kNativeStationheadId = 107;
constexpr int kNativeRadarId = 108;

constexpr COLORREF kWidgetBackground = kNativeDashboardBackground;
constexpr COLORREF kWidgetSurface = RGB(18, 23, 31);
constexpr COLORREF kWidgetSurfaceAlt = RGB(24, 31, 41);
// Label hierarchy follows iOS dark-mode label/secondaryLabel/tertiaryLabel.
constexpr COLORREF kWidgetText = RGB(255, 255, 255);
constexpr COLORREF kWidgetMuted = RGB(152, 152, 157);
constexpr COLORREF kWidgetSubtle = RGB(99, 99, 102);
// Accents follow iOS dark-mode system colors (systemBlue/Green/Orange/Yellow/Red).
constexpr COLORREF kWidgetBlue = RGB(10, 132, 255);
constexpr COLORREF kWidgetBlueMuted = RGB(64, 156, 255);
constexpr COLORREF kWidgetGreen = RGB(48, 209, 88);
constexpr COLORREF kWidgetOrange = RGB(255, 159, 10);
constexpr COLORREF kWidgetPanelHero = RGB(14, 18, 26);
constexpr COLORREF kWidgetStage = RGB(11, 16, 23);
constexpr COLORREF kWidgetTrack = RGB(34, 44, 56);
constexpr COLORREF kWidgetWarning = RGB(255, 214, 10);
constexpr COLORREF kWidgetDanger = RGB(255, 69, 58);
constexpr COLORREF kWidgetDangerSurface = RGB(42, 33, 35);
constexpr COLORREF kWidgetSuccessSurface = RGB(24, 46, 34);

RECT NativeClockRectFromBounds(const RECT& bounds) {
  return ComputeNativeDashboardLayout(bounds).clock;
}

RECT NativeAirRectFromBounds(const RECT& bounds) {
  return ComputeNativeDashboardLayout(bounds).air;
}

RECT NativeAirHistoryRectFromBounds(const RECT& bounds) {
  return ComputeNativeDashboardLayout(bounds).airHistory;
}

RECT NativeControlsRectFromBounds(const RECT& bounds) {
  return ComputeNativeDashboardLayout(bounds).controls;
}

RECT NativeNewsRectFromBounds(const RECT& bounds) {
  return ComputeNativeDashboardLayout(bounds).news;
}

RECT NativeWeatherRectFromBounds(const RECT& bounds) {
  return ComputeNativeDashboardLayout(bounds).weather;
}

RECT NativeEnergyRectFromBounds(const RECT& bounds) {
  return ComputeNativeDashboardLayout(bounds).energy;
}

RECT NativeStationheadRectFromBounds(const RECT& bounds) {
  return ComputeNativeDashboardLayout(bounds).stationhead;
}

RECT NativeRadarRectFromBounds(const RECT& bounds) {
  return ComputeNativeDashboardLayout(bounds).radar;
}

void PlaceNativeWindow(HWND hwnd, const RECT& rect, bool visible) {
  SetWindowPos(hwnd, HWND_TOP, rect.left, rect.top,
               std::max(1L, rect.right - rect.left),
               std::max(1L, rect.bottom - rect.top),
               SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : SWP_NOOWNERZORDER));
  ShowWindow(hwnd, visible ? SW_SHOWNA : SW_HIDE);
}

struct ControlsButtonRects {
  RECT update{};
  RECT restart{};
  int toastTop = 0;
};

ControlsButtonRects ControlsButtonsFromBounds(const RECT& bounds) {
  const int width = std::max(1L, bounds.right - bounds.left);
  const int height = std::max(1L, bounds.bottom - bounds.top);
  if (width < 330) {
    const int buttonHeight = std::clamp(height / 5, 34, 42);
    const int buttonWidth = std::min(190, std::max(120, width - 8));
    const int left = bounds.left + (width - buttonWidth) / 2;
    const int top = bounds.top + std::max(34, (height - buttonHeight * 2 - 8) / 2);
    return ControlsButtonRects{
        RECT{left, top, left + buttonWidth, top + buttonHeight},
        RECT{left, top + buttonHeight + 8, left + buttonWidth, top + buttonHeight * 2 + 8},
        top + buttonHeight * 2 + 18,
    };
  }

  const int buttonWidth = std::min(170, std::max(120, (width - 24) / 2));
  const int buttonHeight = std::clamp(height / 4, 38, 44);
  const int totalWidth = buttonWidth * 2 + 10;
  const int left = bounds.left + (width - totalWidth) / 2;
  const int top = bounds.top + std::max(42, height / 2 - buttonHeight / 2);
  return ControlsButtonRects{
      RECT{left, top, left + buttonWidth, top + buttonHeight},
      RECT{left + buttonWidth + 10, top, left + totalWidth, top + buttonHeight},
      top + buttonHeight + 10,
  };
}

struct StationheadButtonRects {
  RECT primaryAudio{};
  RECT secondaryAudio{};
};

StationheadButtonRects StationheadButtonsFromBounds(const RECT& bounds) {
  const int height = std::max(1L, bounds.bottom - bounds.top);
  const int rowHeight = std::max(56, (height - 34) / 2);
  const int firstTop = bounds.top + 34 + std::max(8, rowHeight / 2 - 15);
  const int secondTop = bounds.top + 34 + rowHeight + std::max(8, rowHeight / 2 - 15);
  const int buttonWidth = std::clamp((bounds.right - bounds.left) / 4, 76L, 94L);
  return StationheadButtonRects{
      RECT{bounds.right - buttonWidth, firstTop, bounds.right, firstTop + 30},
      RECT{bounds.right - buttonWidth, secondTop, bounds.right, secondTop + 30},
  };
}

HFONT CreateUiFont(int height, int weight) {
  return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, L"Yu Gothic UI");
}

std::wstring TrackTimeText(int64_t milliseconds) {
  const int64_t seconds = std::max<int64_t>(0, milliseconds / 1000);
  wchar_t text[32]{};
  swprintf_s(text, L"%lld:%02lld", seconds / 60, seconds % 60);
  return text;
}

void DrawPremultipliedBitmap(HDC dc, HBITMAP bitmap, const RECT& target) {
  if (!bitmap) return;
  const int width = target.right - target.left;
  const int height = target.bottom - target.top;
  if (width <= 0 || height <= 0) return;
  HDC sourceDc = CreateCompatibleDC(dc);
  if (!sourceDc) return;
  HGDIOBJ previousBitmap = SelectObject(sourceDc, bitmap);
  const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  AlphaBlend(dc, target.left, target.top, width, height, sourceDc, 0, 0, width, height, blend);
  SelectObject(sourceDc, previousBitmap);
  DeleteDC(sourceDc);
}

std::wstring DateText(const SYSTEMTIME& now) {
  static constexpr const wchar_t* kWeekdays[] = {L"日", L"月", L"火", L"水", L"木", L"金", L"土"};
  wchar_t text[64]{};
  swprintf_s(text, L"%04u年%u月%u日 (%s)", now.wYear, now.wMonth, now.wDay,
             kWeekdays[now.wDayOfWeek % 7]);
  return text;
}

std::wstring TimeText(const SYSTEMTIME& now) {
  wchar_t text[32]{};
  swprintf_s(text, L"%02u:%02u:%02u", now.wHour, now.wMinute, now.wSecond);
  return text;
}

void DrawTextInRect(HDC dc, const std::wstring& text, RECT rect, int format) {
  DrawTextW(dc, text.c_str(), -1, &rect, format | DT_NOPREFIX);
}

void FillWidgetBackground(HDC dc, const RECT& bounds) {
  HBRUSH background = CreateSolidBrush(kWidgetBackground);
  FillRect(dc, &bounds, background);
  DeleteObject(background);
}

RECT DrawWidgetSurface(HDC dc, const RECT& bounds, COLORREF color = kWidgetSurface) {
  HBRUSH surface = CreateSolidBrush(color);
  HGDIOBJ previousPen = SelectObject(dc, GetStockObject(NULL_PEN));
  HGDIOBJ previousBrush = SelectObject(dc, surface);
  RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom, 20, 20);
  SelectObject(dc, previousBrush);
  SelectObject(dc, previousPen);
  DeleteObject(surface);
  return NormalizeInsetRect(bounds, 16, 14, 16, 14);
}

void DrawWidgetCard(HDC dc, const RECT& rect, COLORREF color = kWidgetSurfaceAlt,
                    int radius = 14) {
  if (rect.right <= rect.left || rect.bottom <= rect.top) return;
  HBRUSH fill = CreateSolidBrush(color);
  HGDIOBJ previousPen = SelectObject(dc, GetStockObject(NULL_PEN));
  HGDIOBJ previousBrush = SelectObject(dc, fill);
  RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
  SelectObject(dc, previousBrush);
  SelectObject(dc, previousPen);
  DeleteObject(fill);
}

void DrawWidgetPill(HDC dc, const RECT& rect, COLORREF color) {
  if (rect.right <= rect.left || rect.bottom <= rect.top) return;
  HBRUSH fill = CreateSolidBrush(color);
  HGDIOBJ previousPen = SelectObject(dc, GetStockObject(NULL_PEN));
  HGDIOBJ previousBrush = SelectObject(dc, fill);
  const int radius = std::max(2L, rect.bottom - rect.top);
  RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
  SelectObject(dc, previousBrush);
  SelectObject(dc, previousPen);
  DeleteObject(fill);
}

void DrawWidgetHeader(HDC dc, const std::wstring& title, const std::wstring& trailing,
                      const RECT& content) {
  HFONT font = CreateUiFont(13, FW_NORMAL);
  HGDIOBJ previousFont = SelectObject(dc, font);
  RECT header{content.left, content.top, content.right, content.top + 23};
  SetTextColor(dc, kWidgetText);
  DrawTextInRect(dc, title, header, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
  if (!trailing.empty()) {
    SetTextColor(dc, kWidgetMuted);
    DrawTextInRect(dc, trailing, header, DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
  }
  SelectObject(dc, previousFont);
  DeleteObject(font);
}

std::wstring Fixed(double value, int digits) {
  std::wostringstream output;
  output << std::fixed << std::setprecision(digits) << value;
  return output.str();
}

std::wstring NumberOrDash(double value, int digits = 0) {
  if (!std::isfinite(value)) return L"--";
  return Fixed(value, digits);
}

double RangeMin(const std::vector<double>& values, double fallback) {
  return values.empty() ? fallback : *std::min_element(values.begin(), values.end());
}

double RangeMax(const std::vector<double>& values, double fallback) {
  return values.empty() ? fallback : *std::max_element(values.begin(), values.end());
}

bool SameSensorSnapshot(const SensorSnapshot& left, const SensorSnapshot& right) {
  return left.co2Connected == right.co2Connected &&
         left.co2 == right.co2 &&
         left.temperatureRaw == right.temperatureRaw &&
         left.humidityRaw == right.humidityRaw &&
         left.temperatureCorrected == right.temperatureCorrected &&
         left.humidityCorrected == right.humidityCorrected &&
         left.observedAt == right.observedAt &&
         left.presence == right.presence &&
         left.light == right.light &&
         left.motion == right.motion &&
         left.doorOpen == right.doorOpen &&
         left.outboxCount == right.outboxCount &&
         left.lastError == right.lastError;
}

bool SameAirHistory(const std::vector<AirHistorySample>& left,
                    const std::vector<AirHistorySample>& right) {
  if (left.size() != right.size()) return false;
  for (size_t index = 0; index < left.size(); ++index) {
    const AirHistorySample& a = left[index];
    const AirHistorySample& b = right[index];
    if (a.timestamp != b.timestamp || a.co2 != b.co2 ||
        a.temperature != b.temperature || a.humidity != b.humidity) {
      return false;
    }
  }
  return true;
}

bool SameNativeStationhead(const StationheadStatus& left, const StationheadStatus& right) {
  return left.created == right.created &&
         left.navigating == right.navigating &&
         left.playing == right.playing &&
         left.loginRequired == right.loginRequired &&
         left.spotifyAuthorization == right.spotifyAuthorization &&
         left.apiAuthorization == right.apiAuthorization &&
         left.lightweight == right.lightweight &&
         left.visible == right.visible &&
         left.processFailed == right.processFailed &&
         left.spotifyConfigured == right.spotifyConfigured &&
         left.authAvailable == right.authAvailable &&
         left.audioPlaying == right.audioPlaying &&
         left.audioSilent == right.audioSilent &&
         left.audioMuted == right.audioMuted &&
         left.secondaryAudioMuted == right.secondaryAudioMuted &&
         left.healthMisses == right.healthMisses &&
         left.lastPlaybackConfirmedAt == right.lastPlaybackConfirmedAt &&
         left.processWorkingSet == right.processWorkingSet &&
         left.processCpuPercent == right.processCpuPercent &&
         left.blockedResources == right.blockedResources &&
         left.url == right.url &&
         left.detail == right.detail &&
         left.trackTitle == right.trackTitle &&
         left.trackArtist == right.trackArtist &&
         left.deviceName == right.deviceName &&
         left.artworkUrl == right.artworkUrl &&
         left.sampledAt == right.sampledAt &&
         left.expectedEndAt == right.expectedEndAt &&
         left.trackDurationMs == right.trackDurationMs;
}

void DrawHistoryLine(HDC dc, const std::vector<AirHistorySample>& samples, const RECT& plot,
                     int64_t cutoff, int64_t spanMs, double minValue, double maxValue,
                     COLORREF color, int width,
                     const std::function<double(const AirHistorySample&)>& valueOf) {
  if (samples.empty() || maxValue <= minValue) return;
  HPEN pen = CreatePen(PS_SOLID, width, color);
  HGDIOBJ previousPen = SelectObject(dc, pen);
  bool started = false;
  for (const auto& sample : samples) {
    const double value = valueOf(sample);
    if (!std::isfinite(value) || sample.timestamp < cutoff) continue;
    const double xRatio = std::clamp(static_cast<double>(sample.timestamp - cutoff) / spanMs, 0.0, 1.0);
    const double yRatio = std::clamp((value - minValue) / (maxValue - minValue), 0.0, 1.0);
    const int x = plot.left + static_cast<int>((plot.right - plot.left) * xRatio);
    const int y = plot.bottom - static_cast<int>((plot.bottom - plot.top) * yRatio);
    if (!started) {
      MoveToEx(dc, x, y, nullptr);
      started = true;
    } else {
      LineTo(dc, x, y);
    }
  }
  SelectObject(dc, previousPen);
  DeleteObject(pen);
}
}  // namespace

bool Renderer::EnsureNativeClockWindow() {
  if (nativeClockWindow_ && IsWindow(nativeClockWindow_)) return true;
  if (!window_ || !IsWindow(window_)) return false;
  static constexpr wchar_t kClockClassName[] = L"HomePanelNativeClock";
  static std::once_flag classOnce;
  std::call_once(classOnce, [] {
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = Renderer::NativeClockWndProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kClockClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    RegisterClassW(&windowClass);
  });

  const RECT rect = NativeClockRectFromBounds(bounds_);
  nativeClockWindow_ = CreateWindowExW(0, kClockClassName, L"HomePanelNativeClock",
      WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE,
      rect.left, rect.top, std::max(1L, rect.right - rect.left),
      std::max(1L, rect.bottom - rect.top), window_, nullptr,
      GetModuleHandleW(nullptr), this);
  if (!nativeClockWindow_ || !IsWindow(nativeClockWindow_)) return false;
  ApplyNativeClockBounds();
  return true;
}

bool Renderer::EnsureNativeStaticWindows() {
  if (!window_ || !IsWindow(window_)) return false;
  static constexpr wchar_t kStaticClassName[] = L"HomePanelNativeStaticPanel";
  static std::once_flag classOnce;
  std::call_once(classOnce, [] {
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = Renderer::NativeStaticWndProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kStaticClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    RegisterClassW(&windowClass);
  });

  if (!nativeAirWindow_ || !IsWindow(nativeAirWindow_)) {
    const RECT rect = NativeAirRectFromBounds(bounds_);
    nativeAirWindow_ = CreateWindowExW(0, kStaticClassName, L"HomePanelNativeAir",
        WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE,
        rect.left, rect.top, std::max(1L, rect.right - rect.left),
        std::max(1L, rect.bottom - rect.top), window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNativeAirId)),
        GetModuleHandleW(nullptr), this);
  }
  if (!nativeAirHistoryWindow_ || !IsWindow(nativeAirHistoryWindow_)) {
    const RECT rect = NativeAirHistoryRectFromBounds(bounds_);
    nativeAirHistoryWindow_ = CreateWindowExW(0, kStaticClassName, L"HomePanelNativeAirHistory",
        WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE,
        rect.left, rect.top, std::max(1L, rect.right - rect.left),
        std::max(1L, rect.bottom - rect.top), window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNativeAirHistoryId)),
        GetModuleHandleW(nullptr), this);
  }
  if (!nativeControlsWindow_ || !IsWindow(nativeControlsWindow_)) {
    const RECT rect = NativeControlsRectFromBounds(bounds_);
    nativeControlsWindow_ = CreateWindowExW(0, kStaticClassName, L"HomePanelNativeControls",
        WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE,
        rect.left, rect.top, std::max(1L, rect.right - rect.left),
        std::max(1L, rect.bottom - rect.top), window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNativeControlsId)),
        GetModuleHandleW(nullptr), this);
  }
  if (!nativeNewsWindow_ || !IsWindow(nativeNewsWindow_)) {
    const RECT rect = NativeNewsRectFromBounds(bounds_);
    nativeNewsWindow_ = CreateWindowExW(0, kStaticClassName, L"HomePanelNativeNews",
        WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE,
        rect.left, rect.top, std::max(1L, rect.right - rect.left),
        std::max(1L, rect.bottom - rect.top), window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNativeNewsId)),
        GetModuleHandleW(nullptr), this);
  }
  if (!nativeWeatherWindow_ || !IsWindow(nativeWeatherWindow_)) {
    const RECT rect = NativeWeatherRectFromBounds(bounds_);
    nativeWeatherWindow_ = CreateWindowExW(0, kStaticClassName, L"HomePanelNativeWeather",
        WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE,
        rect.left, rect.top, std::max(1L, rect.right - rect.left),
        std::max(1L, rect.bottom - rect.top), window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNativeWeatherId)),
        GetModuleHandleW(nullptr), this);
  }
  if (!nativeEnergyWindow_ || !IsWindow(nativeEnergyWindow_)) {
    const RECT rect = NativeEnergyRectFromBounds(bounds_);
    nativeEnergyWindow_ = CreateWindowExW(0, kStaticClassName, L"HomePanelNativeEnergy",
        WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE,
        rect.left, rect.top, std::max(1L, rect.right - rect.left),
        std::max(1L, rect.bottom - rect.top), window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNativeEnergyId)),
        GetModuleHandleW(nullptr), this);
  }
  if (!nativeStationheadWindow_ || !IsWindow(nativeStationheadWindow_)) {
    const RECT rect = NativeStationheadRectFromBounds(bounds_);
    nativeStationheadWindow_ = CreateWindowExW(0, kStaticClassName, L"HomePanelNativeStationhead",
        WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE,
        rect.left, rect.top, std::max(1L, rect.right - rect.left),
        std::max(1L, rect.bottom - rect.top), window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNativeStationheadId)),
        GetModuleHandleW(nullptr), this);
  }
  if (!nativeRadarWindow_ || !IsWindow(nativeRadarWindow_)) {
    const RECT rect = NativeRadarRectFromBounds(bounds_);
    nativeRadarWindow_ = CreateWindowExW(0, kStaticClassName, L"HomePanelNativeRadar",
        WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE,
        rect.left, rect.top, std::max(1L, rect.right - rect.left),
        std::max(1L, rect.bottom - rect.top), window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNativeRadarId)),
        GetModuleHandleW(nullptr), this);
  }
  ApplyNativeStaticBounds();
  return nativeAirWindow_ && nativeAirHistoryWindow_ && nativeControlsWindow_ && nativeNewsWindow_ &&
         nativeWeatherWindow_ && nativeEnergyWindow_ && nativeStationheadWindow_ && nativeRadarWindow_;
}

void Renderer::ApplyNativeStaticBounds() {
  const NativeDashboardLayout layout = ComputeNativeDashboardLayout(bounds_);
  if (nativeAirWindow_ && IsWindow(nativeAirWindow_)) {
    PlaceNativeWindow(nativeAirWindow_, layout.air, nativeDashboardVisible_);
    InvalidateRect(nativeAirWindow_, nullptr, FALSE);
  }
  if (nativeAirHistoryWindow_ && IsWindow(nativeAirHistoryWindow_)) {
    PlaceNativeWindow(nativeAirHistoryWindow_, layout.airHistory, nativeDashboardVisible_);
    InvalidateRect(nativeAirHistoryWindow_, nullptr, FALSE);
  }
  if (nativeControlsWindow_ && IsWindow(nativeControlsWindow_)) {
    PlaceNativeWindow(nativeControlsWindow_, layout.controls, nativeDashboardVisible_);
    InvalidateRect(nativeControlsWindow_, nullptr, FALSE);
  }
  if (nativeNewsWindow_ && IsWindow(nativeNewsWindow_)) {
    PlaceNativeWindow(nativeNewsWindow_, layout.news, nativeDashboardVisible_);
    InvalidateRect(nativeNewsWindow_, nullptr, FALSE);
  }
  if (nativeWeatherWindow_ && IsWindow(nativeWeatherWindow_)) {
    PlaceNativeWindow(nativeWeatherWindow_, layout.weather, nativeDashboardVisible_);
    InvalidateRect(nativeWeatherWindow_, nullptr, FALSE);
  }
  if (nativeEnergyWindow_ && IsWindow(nativeEnergyWindow_)) {
    PlaceNativeWindow(nativeEnergyWindow_, layout.energy, nativeDashboardVisible_);
    InvalidateRect(nativeEnergyWindow_, nullptr, FALSE);
  }
  if (nativeStationheadWindow_ && IsWindow(nativeStationheadWindow_)) {
    PlaceNativeWindow(nativeStationheadWindow_, layout.stationhead, nativeDashboardVisible_);
    InvalidateRect(nativeStationheadWindow_, nullptr, FALSE);
  }
  if (nativeRadarWindow_ && IsWindow(nativeRadarWindow_)) {
    PlaceNativeWindow(nativeRadarWindow_, layout.radar, nativeDashboardVisible_);
    InvalidateRect(nativeRadarWindow_, nullptr, FALSE);
  }
}

void Renderer::DestroyNativeStaticWindows() {
  if (nativeAirWindow_ && IsWindow(nativeAirWindow_)) DestroyWindow(nativeAirWindow_);
  if (nativeAirHistoryWindow_ && IsWindow(nativeAirHistoryWindow_)) DestroyWindow(nativeAirHistoryWindow_);
  if (nativeControlsWindow_ && IsWindow(nativeControlsWindow_)) DestroyWindow(nativeControlsWindow_);
  if (nativeNewsWindow_ && IsWindow(nativeNewsWindow_)) DestroyWindow(nativeNewsWindow_);
  if (nativeWeatherWindow_ && IsWindow(nativeWeatherWindow_)) DestroyWindow(nativeWeatherWindow_);
  if (nativeEnergyWindow_ && IsWindow(nativeEnergyWindow_)) DestroyWindow(nativeEnergyWindow_);
  if (nativeStationheadWindow_ && IsWindow(nativeStationheadWindow_)) {
    DestroyWindow(nativeStationheadWindow_);
  }
  if (nativeRadarWindow_ && IsWindow(nativeRadarWindow_)) DestroyWindow(nativeRadarWindow_);
  for (auto& [hwnd, buffer] : nativeBackBuffers_) {
    if (buffer.bitmap) DeleteObject(buffer.bitmap);
  }
  nativeBackBuffers_.clear();
  for (auto& [key, entry] : nativeArtworkBitmaps_) {
    if (entry.bitmap) DeleteObject(entry.bitmap);
  }
  nativeArtworkBitmaps_.clear();
  nativeArtworkUseCounter_ = 0;
  {
    std::lock_guard lock(radarFrameMutex_);
    if (radarFrameBitmap_) DeleteObject(radarFrameBitmap_);
    radarFrameBitmap_ = nullptr;
  }
  nativeAirWindow_ = nullptr;
  nativeAirHistoryWindow_ = nullptr;
  nativeControlsWindow_ = nullptr;
  nativeNewsWindow_ = nullptr;
  nativeWeatherWindow_ = nullptr;
  nativeEnergyWindow_ = nullptr;
  nativeStationheadWindow_ = nullptr;
  nativeRadarWindow_ = nullptr;
}

void Renderer::UpdateNativeStaticPanels(const RenderState& state) {
  const bool sensorsChanged = !SameSensorSnapshot(nativeSensors_, state.sensors);
  const bool historyChanged = !SameAirHistory(nativeAirHistory_, state.airHistory);
  const bool stationheadChanged = !SameNativeStationhead(nativeStationhead_, state.stationhead);
  const bool controlsChanged = nativeAppVersion_ != state.appVersion || nativeToast_ != state.toast;
  const bool newsChanged = nativeNewsIndex_ != state.newsIndex;
  const bool dashboardChanged = nativeRenderedDashboardRevision_ != dashboardSourceRevision_;

  nativeSensors_ = state.sensors;
  nativeAirHistory_ = state.airHistory;
  nativeStationhead_ = state.stationhead;
  nativeAppVersion_ = state.appVersion;
  nativeToast_ = state.toast;
  nativeNewsIndex_ = state.newsIndex;
  nativeRenderedDashboardRevision_ = dashboardSourceRevision_;
  if (!EnsureNativeStaticWindows()) return;
  if (sensorsChanged) InvalidateRect(nativeAirWindow_, nullptr, FALSE);
  if (sensorsChanged || historyChanged) InvalidateRect(nativeAirHistoryWindow_, nullptr, FALSE);
  if (controlsChanged) InvalidateRect(nativeControlsWindow_, nullptr, FALSE);
  if (dashboardChanged || newsChanged) InvalidateRect(nativeNewsWindow_, nullptr, FALSE);
  if (dashboardChanged) {
    InvalidateRect(nativeWeatherWindow_, nullptr, FALSE);
    InvalidateRect(nativeEnergyWindow_, nullptr, FALSE);
  }
  if (stationheadChanged) InvalidateRect(nativeStationheadWindow_, nullptr, FALSE);
}

void Renderer::TickNativePanels(int64_t nowMs) {
  if (!nativeDashboardVisible_) return;
  if (nativeClockWindow_ && IsWindow(nativeClockWindow_) && IsWindowVisible(nativeClockWindow_)) {
    InvalidateRect(nativeClockWindow_, nullptr, FALSE);
  }
  if (nativeStationheadWindow_ && IsWindow(nativeStationheadWindow_) &&
      IsWindowVisible(nativeStationheadWindow_) && NativePlaybackActive(nowMs)) {
    InvalidateRect(nativeStationheadWindow_, nullptr, FALSE);
  }
}

HBITMAP Renderer::NativePanelBackBuffer(HWND hwnd, HDC dc, int width, int height) {
  if (!hwnd || !dc || width <= 0 || height <= 0) return nullptr;
  NativeBackBuffer& buffer = nativeBackBuffers_[hwnd];
  if (buffer.bitmap && buffer.width == width && buffer.height == height) return buffer.bitmap;
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

void Renderer::ApplyNativeClockBounds() {
  if (!nativeClockWindow_ || !IsWindow(nativeClockWindow_)) return;
  const RECT rect = ComputeNativeDashboardLayout(bounds_).clock;
  PlaceNativeWindow(nativeClockWindow_, rect, nativeDashboardVisible_);
  InvalidateRect(nativeClockWindow_, nullptr, FALSE);
}

void Renderer::DestroyNativeClockWindow() {
  if (nativeClockWindow_ && IsWindow(nativeClockWindow_)) {
    DestroyWindow(nativeClockWindow_);
  }
  nativeClockWindow_ = nullptr;
}

LRESULT CALLBACK Renderer::NativeClockWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  Renderer* renderer = reinterpret_cast<Renderer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    auto* createstruct = reinterpret_cast<CREATESTRUCTW*>(lparam);
    renderer = reinterpret_cast<Renderer*>(createstruct->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(renderer));
  }
  if (renderer) return renderer->HandleNativeClockMessage(hwnd, message, wparam, lparam);
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK Renderer::NativeStaticWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  Renderer* renderer = reinterpret_cast<Renderer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    auto* createstruct = reinterpret_cast<CREATESTRUCTW*>(lparam);
    renderer = reinterpret_cast<Renderer*>(createstruct->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(renderer));
  }
  if (renderer) return renderer->HandleNativeStaticMessage(hwnd, message, wparam, lparam);
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT Renderer::HandleNativeClockMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      PaintNativeClock(hwnd);
      return 0;
    case WM_NCDESTROY:
      ReleaseNativePanelBackBuffer(hwnd);
      if (nativeClockWindow_ == hwnd) nativeClockWindow_ = nullptr;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      break;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT Renderer::HandleNativeStaticMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_ERASEBKGND:
      return 1;
    case WM_LBUTTONUP:
      if (GetDlgCtrlID(hwnd) == kNativeControlsId) {
        RECT bounds{};
        GetClientRect(hwnd, &bounds);
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const ControlsButtonRects buttons = ControlsButtonsFromBounds(bounds);
        if (PtInRect(&buttons.update, point)) QueueAction(UiAction::AppUpdate);
        else if (PtInRect(&buttons.restart, point)) QueueAction(UiAction::Restart);
        return 0;
      }
      if (GetDlgCtrlID(hwnd) == kNativeStationheadId) {
        RECT bounds{};
        GetClientRect(hwnd, &bounds);
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const StationheadButtonRects buttons = StationheadButtonsFromBounds(bounds);
        if (PtInRect(&buttons.primaryAudio, point)) QueueAction(UiAction::StationheadAudioToggleA);
        else if (PtInRect(&buttons.secondaryAudio, point)) QueueAction(UiAction::StationheadAudioToggleB);
        return 0;
      }
      break;
    case WM_PAINT:
      if (GetDlgCtrlID(hwnd) == kNativeAirId) PaintNativeAir(hwnd);
      else if (GetDlgCtrlID(hwnd) == kNativeAirHistoryId) PaintNativeAirHistory(hwnd);
      else if (GetDlgCtrlID(hwnd) == kNativeControlsId) PaintNativeControls(hwnd);
      else if (GetDlgCtrlID(hwnd) == kNativeNewsId) PaintNativeNews(hwnd);
      else if (GetDlgCtrlID(hwnd) == kNativeWeatherId) PaintNativeWeather(hwnd);
      else if (GetDlgCtrlID(hwnd) == kNativeEnergyId) PaintNativeEnergy(hwnd);
      else if (GetDlgCtrlID(hwnd) == kNativeRadarId) PaintNativeRadar(hwnd);
      else PaintNativeStationhead(hwnd);
      return 0;
    case WM_NCDESTROY:
      ReleaseNativePanelBackBuffer(hwnd);
      if (nativeAirWindow_ == hwnd) nativeAirWindow_ = nullptr;
      if (nativeAirHistoryWindow_ == hwnd) nativeAirHistoryWindow_ = nullptr;
      if (nativeControlsWindow_ == hwnd) nativeControlsWindow_ = nullptr;
      if (nativeNewsWindow_ == hwnd) nativeNewsWindow_ = nullptr;
      if (nativeWeatherWindow_ == hwnd) nativeWeatherWindow_ = nullptr;
      if (nativeEnergyWindow_ == hwnd) nativeEnergyWindow_ = nullptr;
      if (nativeStationheadWindow_ == hwnd) nativeStationheadWindow_ = nullptr;
      if (nativeRadarWindow_ == hwnd) nativeRadarWindow_ = nullptr;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      break;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

void Renderer::PaintNativeClock(HWND hwnd) {
  PAINTSTRUCT paint{};
  HDC dc = BeginPaint(hwnd, &paint);
  if (!dc) return;

  RECT bounds{};
  GetClientRect(hwnd, &bounds);
  HDC memoryDc = CreateCompatibleDC(dc);
  HBITMAP bitmap = NativePanelBackBuffer(hwnd, dc, std::max(1L, bounds.right), std::max(1L, bounds.bottom));
  HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);
  FillWidgetBackground(memoryDc, bounds);
  const RECT content = DrawWidgetSurface(memoryDc, bounds, kWidgetPanelHero);

  SetBkMode(memoryDc, TRANSPARENT);
  SetTextColor(memoryDc, kWidgetMuted);

  SYSTEMTIME now{};
  GetLocalTime(&now);
  const int height = std::max(1L, content.bottom - content.top);
  const int dateHeight = std::clamp(height / 8, 14, 22);
  const int clockHeight = std::clamp(height / 2, 52, 86);

  RECT dateRect = content;
  dateRect.bottom = content.top + dateHeight + 6;
  HFONT dateFont = CreateUiFont(dateHeight, FW_NORMAL);
  HGDIOBJ previousFont = SelectObject(memoryDc, dateFont);
  DrawTextW(memoryDc, DateText(now).c_str(), -1, &dateRect,
            DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
  SelectObject(memoryDc, previousFont);
  DeleteObject(dateFont);

  SetTextColor(memoryDc, kWidgetText);
  RECT timeRect = content;
  timeRect.top = dateRect.bottom + std::max(2, height / 12);
  HFONT clockFont = CreateUiFont(clockHeight, FW_LIGHT);
  previousFont = SelectObject(memoryDc, clockFont);
  DrawTextW(memoryDc, TimeText(now).c_str(), -1, &timeRect,
            DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
  SelectObject(memoryDc, previousFont);
  DeleteObject(clockFont);

  BitBlt(dc, 0, 0, bounds.right, bounds.bottom, memoryDc, 0, 0, SRCCOPY);
  SelectObject(memoryDc, previousBitmap);
  DeleteDC(memoryDc);
  EndPaint(hwnd, &paint);
}

void Renderer::PaintNativeAir(HWND hwnd) {
  PAINTSTRUCT paint{};
  HDC dc = BeginPaint(hwnd, &paint);
  if (!dc) return;

  RECT bounds{};
  GetClientRect(hwnd, &bounds);
  HDC memoryDc = CreateCompatibleDC(dc);
  HBITMAP bitmap = NativePanelBackBuffer(hwnd, dc, std::max(1L, bounds.right), std::max(1L, bounds.bottom));
  HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);
  FillWidgetBackground(memoryDc, bounds);
  SetBkMode(memoryDc, TRANSPARENT);
  const RECT content = DrawWidgetSurface(memoryDc, bounds, kWidgetSurface);

  const int width = std::max(1L, content.right - content.left);
  const int height = std::max(1L, content.bottom - content.top);
  const int gap = 16;
  const std::array<std::pair<std::wstring, std::wstring>, 3> values{{
      {L"CO2", nativeSensors_.co2Connected ? std::to_wstring(nativeSensors_.co2) + L" ppm" : L"--- ppm"},
      {L"温度", nativeSensors_.co2Connected ? Fixed(nativeSensors_.temperatureCorrected, 1) + L"℃" : L"--.-℃"},
      {L"湿度", nativeSensors_.co2Connected ? Fixed(nativeSensors_.humidityCorrected, 0) + L"%" : L"--%"},
  }};
  HFONT labelFont = CreateUiFont(std::clamp(height / 5, 10, 15), FW_NORMAL);
  HFONT valueFont = CreateUiFont(std::clamp(height / 3, 18, 29), FW_SEMIBOLD);
  const bool compact = width < 230 && height >= 56;
  for (int i = 0; i < 3; ++i) {
    RECT rect{};
    if (compact && i == 0) {
      rect = RECT{content.left, content.top, content.right, content.top + std::max(28, height / 2)};
    } else if (compact) {
      const int lowerTop = content.top + std::max(28, height / 2) + gap;
      const int lowerWidth = (width - gap) / 2;
      rect = RECT{content.left + (i - 1) * (lowerWidth + gap), lowerTop,
                  content.left + (i - 1) * (lowerWidth + gap) + lowerWidth, content.bottom};
    } else {
      const int cardWidth = (width - gap * 2) / 3;
      rect = RECT{content.left + i * (cardWidth + gap), content.top,
                  content.left + i * (cardWidth + gap) + cardWidth, content.bottom};
    }
    RECT labelRect{rect.left, rect.top + 6, rect.right, rect.top + std::clamp(height / 3, 20, 26)};
    SetTextColor(memoryDc, kWidgetMuted);
    HGDIOBJ previousFont = SelectObject(memoryDc, labelFont);
    DrawTextInRect(memoryDc, values[i].first, labelRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    RECT valueRect{rect.left + 2, labelRect.bottom, rect.right - 2, rect.bottom - 4};
    SetTextColor(memoryDc, kWidgetText);
    SelectObject(memoryDc, valueFont);
    DrawTextInRect(memoryDc, values[i].second, valueRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(memoryDc, previousFont);
  }
  DeleteObject(labelFont);
  DeleteObject(valueFont);

  BitBlt(dc, 0, 0, bounds.right, bounds.bottom, memoryDc, 0, 0, SRCCOPY);
  SelectObject(memoryDc, previousBitmap);
  DeleteDC(memoryDc);
  EndPaint(hwnd, &paint);
}

void Renderer::PaintNativeAirHistory(HWND hwnd) {
  PAINTSTRUCT paint{};
  HDC dc = BeginPaint(hwnd, &paint);
  if (!dc) return;

  RECT bounds{};
  GetClientRect(hwnd, &bounds);
  HDC memoryDc = CreateCompatibleDC(dc);
  HBITMAP bitmap = NativePanelBackBuffer(hwnd, dc, std::max(1L, bounds.right), std::max(1L, bounds.bottom));
  HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);
  FillWidgetBackground(memoryDc, bounds);
  SetBkMode(memoryDc, TRANSPARENT);
  const RECT content = DrawWidgetSurface(memoryDc, bounds, kWidgetSurface);

  const int64_t now = UnixMillis();
  constexpr int64_t kWindowMs = 24LL * 60 * 60 * 1000;
  const int64_t cutoff = now - kWindowMs;
  std::vector<AirHistorySample> samples;
  std::vector<double> co2Values;
  std::vector<double> temperatureValues;
  std::vector<double> humidityValues;
  for (const auto& sample : nativeAirHistory_) {
    if (sample.timestamp < cutoff || sample.co2 < 250 || sample.co2 > 10000 ||
        sample.temperature < -40 || sample.temperature > 85 ||
        sample.humidity < 0 || sample.humidity > 100) {
      continue;
    }
    samples.push_back(sample);
    co2Values.push_back(sample.co2);
    temperatureValues.push_back(sample.temperature);
    humidityValues.push_back(sample.humidity);
  }

  HFONT font = CreateUiFont(11, FW_NORMAL);
  HGDIOBJ previousFont = SelectObject(memoryDc, font);
  SetTextColor(memoryDc, kWidgetMuted);
  RECT legend{content.left, content.top, content.right, content.top + 18};
  DrawTextInRect(memoryDc, L"CO2   温度   湿度", legend, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

  RECT plot{content.left + 26, content.top + 22, content.right, content.bottom - 20};
  HPEN gridPen = CreatePen(PS_SOLID, 1, kWidgetTrack);
  HGDIOBJ previousPen = SelectObject(memoryDc, gridPen);
  for (int i = 0; i < 4; ++i) {
    const int y = plot.top + (plot.bottom - plot.top) * i / 3;
    MoveToEx(memoryDc, plot.left, y, nullptr);
    LineTo(memoryDc, plot.right, y);
  }
  SelectObject(memoryDc, previousPen);
  DeleteObject(gridPen);

  if (!samples.empty() && plot.bottom > plot.top + 4) {
    const double co2Min = RangeMin(co2Values, 400) - 80;
    const double co2Max = RangeMax(co2Values, 1000) + 80;
    const double tempMin = RangeMin(temperatureValues, 20) - 0.5;
    const double tempMax = RangeMax(temperatureValues, 28) + 0.5;
    const double humMin = RangeMin(humidityValues, 30) - 2;
    const double humMax = RangeMax(humidityValues, 80) + 2;
    DrawHistoryLine(memoryDc, samples, plot, cutoff, kWindowMs, co2Min, co2Max, kWidgetGreen, 2,
                    [](const AirHistorySample& s) { return static_cast<double>(s.co2); });
    DrawHistoryLine(memoryDc, samples, plot, cutoff, kWindowMs, tempMin, tempMax, kWidgetOrange, 1,
                    [](const AirHistorySample& s) { return s.temperature; });
    DrawHistoryLine(memoryDc, samples, plot, cutoff, kWindowMs, humMin, humMax, kWidgetBlue, 1,
                    [](const AirHistorySample& s) { return s.humidity; });
  } else {
    RECT empty = content;
    DrawTextInRect(memoryDc, L"履歴を取得中", empty, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  }

  SetTextColor(memoryDc, kWidgetSubtle);
  RECT axis{content.left, content.bottom - 18, content.right, content.bottom};
  DrawTextInRect(memoryDc, L"24時間前", axis, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  DrawTextInRect(memoryDc, L"現在", axis, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
  SelectObject(memoryDc, previousFont);
  DeleteObject(font);

  BitBlt(dc, 0, 0, bounds.right, bounds.bottom, memoryDc, 0, 0, SRCCOPY);
  SelectObject(memoryDc, previousBitmap);
  DeleteDC(memoryDc);
  EndPaint(hwnd, &paint);
}

void Renderer::PaintNativeControls(HWND hwnd) {
  PAINTSTRUCT paint{};
  HDC dc = BeginPaint(hwnd, &paint);
  if (!dc) return;

  RECT bounds{};
  GetClientRect(hwnd, &bounds);
  HDC memoryDc = CreateCompatibleDC(dc);
  HBITMAP bitmap = NativePanelBackBuffer(hwnd, dc, std::max(1L, bounds.right), std::max(1L, bounds.bottom));
  HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);
  FillWidgetBackground(memoryDc, bounds);
  SetBkMode(memoryDc, TRANSPARENT);
  const RECT content = DrawWidgetSurface(memoryDc, bounds);

  const std::wstring version = nativeAppVersion_.empty() ? L"" : L"v" + nativeAppVersion_;
  const int contentHeight = std::max(1L, content.bottom - content.top);
  if (contentHeight >= 70) DrawWidgetHeader(memoryDc, L"操作", version, content);

  const ControlsButtonRects controlButtons = ControlsButtonsFromBounds(content);
  const std::array<std::pair<std::wstring, RECT>, 2> buttons{{
      {L"更新", controlButtons.update},
      {L"再起動", controlButtons.restart},
  }};
  const int buttonHeight = std::max(1L, controlButtons.update.bottom - controlButtons.update.top);
  HFONT buttonFont = CreateUiFont(std::clamp(buttonHeight / 3, 12, 14), FW_SEMIBOLD);
  HGDIOBJ previousFont = SelectObject(memoryDc, buttonFont);
  SetTextColor(memoryDc, kWidgetText);
  for (const auto& [label, rect] : buttons) {
    DrawWidgetCard(memoryDc, rect, kWidgetSurfaceAlt);
    RECT textRect = rect;
    DrawTextInRect(memoryDc, label, textRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  }
  SelectObject(memoryDc, previousFont);
  DeleteObject(buttonFont);
  if (!nativeToast_.empty()) {
    HFONT toastFont = CreateUiFont(12, FW_NORMAL);
    previousFont = SelectObject(memoryDc, toastFont);
    SetTextColor(memoryDc, kWidgetWarning);
    RECT toastRect{content.left, controlButtons.toastTop, content.right,
                   std::max(static_cast<LONG>(controlButtons.toastTop + 22), content.bottom)};
    DrawTextInRect(memoryDc, nativeToast_, toastRect,
                   DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
    SelectObject(memoryDc, previousFont);
    DeleteObject(toastFont);
  }
  BitBlt(dc, 0, 0, bounds.right, bounds.bottom, memoryDc, 0, 0, SRCCOPY);
  SelectObject(memoryDc, previousBitmap);
  DeleteDC(memoryDc);
  EndPaint(hwnd, &paint);
}

void Renderer::PaintNativeNews(HWND hwnd) {
  PAINTSTRUCT paint{};
  HDC dc = BeginPaint(hwnd, &paint);
  if (!dc) return;

  RECT bounds{};
  GetClientRect(hwnd, &bounds);
  HDC memoryDc = CreateCompatibleDC(dc);
  HBITMAP bitmap = NativePanelBackBuffer(hwnd, dc, std::max(1L, bounds.right), std::max(1L, bounds.bottom));
  HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);
  FillWidgetBackground(memoryDc, bounds);
  SetBkMode(memoryDc, TRANSPARENT);
  const RECT content = DrawWidgetSurface(memoryDc, bounds, kWidgetSurface);

  const NewsItemData* item = nullptr;
  if (!nativeDashboard_.newsItems.empty()) {
    const size_t index = static_cast<size_t>(std::max(0, nativeNewsIndex_)) % nativeDashboard_.newsItems.size();
    item = &nativeDashboard_.newsItems[index];
  }
  const std::wstring title = item ? item->title : L"ニュース取得待ち";
  const std::wstring detail = item ? item->description : L"";

  const int contentHeight = std::max(1L, content.bottom - content.top);
  HFONT titleFont = CreateUiFont(std::clamp(contentHeight / 6, 14, 18), FW_SEMIBOLD);
  HGDIOBJ previousFont = SelectObject(memoryDc, titleFont);
  SetTextColor(memoryDc, kWidgetText);
  RECT titleRect{content.left, content.top, content.right,
                 content.top + std::clamp(contentHeight / 4, 24, 34)};
  DrawTextInRect(memoryDc, title, titleRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
  SelectObject(memoryDc, previousFont);
  DeleteObject(titleFont);

  HFONT detailFont = CreateUiFont(std::clamp(contentHeight / 10, 10, 12), FW_NORMAL);
  previousFont = SelectObject(memoryDc, detailFont);
  SetTextColor(memoryDc, kWidgetMuted);
  RECT detailRect{content.left, titleRect.bottom + 4, content.right, content.bottom};
  DrawTextInRect(memoryDc, detail, detailRect, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
  SelectObject(memoryDc, previousFont);
  DeleteObject(detailFont);

  BitBlt(dc, 0, 0, bounds.right, bounds.bottom, memoryDc, 0, 0, SRCCOPY);
  SelectObject(memoryDc, previousBitmap);
  DeleteDC(memoryDc);
  EndPaint(hwnd, &paint);
}

void Renderer::PaintNativeWeather(HWND hwnd) {
  PAINTSTRUCT paint{};
  HDC dc = BeginPaint(hwnd, &paint);
  if (!dc) return;

  RECT bounds{};
  GetClientRect(hwnd, &bounds);
  HDC memoryDc = CreateCompatibleDC(dc);
  HBITMAP bitmap = NativePanelBackBuffer(hwnd, dc, std::max(1L, bounds.right), std::max(1L, bounds.bottom));
  HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);
  FillWidgetBackground(memoryDc, bounds);
  SetBkMode(memoryDc, TRANSPARENT);
  const RECT content = DrawWidgetSurface(memoryDc, bounds, kWidgetSurface);

  const auto& hours = nativeDashboard_.weatherHours;
  const size_t count = std::min<size_t>(5, hours.size());
  double maxPop = 0;
  for (size_t i = 0; i < count; ++i) {
    if (std::isfinite(hours[i].precipitationProbability)) maxPop = std::max(maxPop, hours[i].precipitationProbability);
  }

  const int contentWidth = std::max(1L, content.right - content.left);
  const int popWidth = std::clamp(contentWidth / 4, 58, 72);
  RECT popRect{content.left, content.top, content.left + popWidth, content.bottom};

  const int popHeight = std::max(1, static_cast<int>(popRect.bottom - popRect.top));
  HFONT smallFont = CreateUiFont(std::clamp(popHeight / 9, 8, 10), FW_NORMAL);
  HGDIOBJ previousFont = SelectObject(memoryDc, smallFont);
  SetTextColor(memoryDc, kWidgetMuted);
  RECT labelRect{popRect.left, popRect.top + 14, popRect.right, popRect.top + 32};
  DrawTextInRect(memoryDc, L"降水確率", labelRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  SelectObject(memoryDc, previousFont);
  DeleteObject(smallFont);

  HFONT popFont = CreateUiFont(std::clamp(popHeight / 3, 20, 30), FW_BOLD);
  previousFont = SelectObject(memoryDc, popFont);
  SetTextColor(memoryDc, maxPop >= 70 ? kWidgetBlue : maxPop >= 40 ? kWidgetBlueMuted : kWidgetMuted);
  RECT valueRect{popRect.left, popRect.top + 36, popRect.right, popRect.bottom - 10};
  DrawTextInRect(memoryDc, std::to_wstring(static_cast<int>(std::round(maxPop))) + L"%", valueRect,
                 DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  SelectObject(memoryDc, previousFont);
  DeleteObject(popFont);

  HFONT hourFont = CreateUiFont(10, FW_NORMAL);
  HFONT rainFont = CreateUiFont(13, FW_NORMAL);
  const int rightLeft = popRect.right + 18;
  const int cardGap = 12;
  const int availableWidth = std::max(1L, content.right - rightLeft);
  const int slotCount = std::clamp(availableWidth / 42, 2, 5);
  const int cardWidth = std::max(1, (availableWidth - cardGap * (slotCount - 1)) / slotCount);
  for (int i = 0; i < slotCount; ++i) {
    RECT cardRect{rightLeft + i * (cardWidth + cardGap), content.top,
                  rightLeft + i * (cardWidth + cardGap) + cardWidth, content.bottom};
    SetTextColor(memoryDc, kWidgetMuted);
    previousFont = SelectObject(memoryDc, hourFont);
    RECT hourRect{cardRect.left, cardRect.top + 8, cardRect.right, cardRect.top + 24};
    const std::wstring hour = i < static_cast<int>(count) ? std::to_wstring(hours[i].hour) + L"時" : L"--";
    DrawTextInRect(memoryDc, hour, hourRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SetTextColor(memoryDc, kWidgetBlue);
    SelectObject(memoryDc, rainFont);
    RECT rainRect{cardRect.left, cardRect.top + 45, cardRect.right, cardRect.bottom - 8};
    const std::wstring rain = i < static_cast<int>(count) ? NumberOrDash(hours[i].rainMm, 0) + L"mm" : L"--";
    DrawTextInRect(memoryDc, rain, rainRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(memoryDc, previousFont);
  }
  DeleteObject(hourFont);
  DeleteObject(rainFont);

  BitBlt(dc, 0, 0, bounds.right, bounds.bottom, memoryDc, 0, 0, SRCCOPY);
  SelectObject(memoryDc, previousBitmap);
  DeleteDC(memoryDc);
  EndPaint(hwnd, &paint);
}

void Renderer::PaintNativeEnergy(HWND hwnd) {
  PAINTSTRUCT paint{};
  HDC dc = BeginPaint(hwnd, &paint);
  if (!dc) return;

  RECT bounds{};
  GetClientRect(hwnd, &bounds);
  HDC memoryDc = CreateCompatibleDC(dc);
  HBITMAP bitmap = NativePanelBackBuffer(hwnd, dc, std::max(1L, bounds.right), std::max(1L, bounds.bottom));
  HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);
  FillWidgetBackground(memoryDc, bounds);
  SetBkMode(memoryDc, TRANSPARENT);
  const RECT content = DrawWidgetSurface(memoryDc, bounds);

  DrawWidgetHeader(memoryDc, L"Octopus Energy", L"", content);

  const auto usageText = [](double value) {
    return std::isfinite(value) ? Fixed(value, 1) + L" kWh" : L"-- kWh";
  };
  const auto costText = [](double value) {
    if (!std::isfinite(value) || value <= 0) return std::wstring{};
    return L"約¥" + std::to_wstring(static_cast<int>(std::round(value * 32.0)));
  };

  const std::array<std::tuple<std::wstring, std::wstring, std::wstring>, 2> summary{{
      {L"先月", usageText(nativeDashboard_.lastMonthUsage), costText(nativeDashboard_.lastMonthUsage)},
      {L"今月見込み", usageText(nativeDashboard_.projectedUsage), costText(nativeDashboard_.projectedUsage)},
  }};

  const int contentHeight = std::max(1L, content.bottom - content.top);
  const int summaryHeight = std::clamp(contentHeight / 3, 58, 72);
  HFONT labelFont = CreateUiFont(std::clamp(summaryHeight / 6, 9, 11), FW_NORMAL);
  HFONT valueFont = CreateUiFont(std::clamp(summaryHeight / 4, 16, 20), FW_SEMIBOLD);
  HGDIOBJ previousFont = nullptr;
  const int summaryGap = 20;
  const int summaryWidth = (std::max(1L, content.right - content.left) - summaryGap) / 2;
  for (int i = 0; i < 2; ++i) {
    RECT rect{content.left + i * (summaryWidth + summaryGap), content.top + 30,
              content.left + i * (summaryWidth + summaryGap) + summaryWidth,
              content.top + 30 + summaryHeight};
    SetTextColor(memoryDc, kWidgetMuted);
    previousFont = SelectObject(memoryDc, labelFont);
    RECT labelRect{rect.left, rect.top + 7, rect.right, rect.top + 24};
    DrawTextInRect(memoryDc, std::get<0>(summary[i]), labelRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SetTextColor(memoryDc, kWidgetText);
    SelectObject(memoryDc, valueFont);
    RECT valueRect{rect.left, rect.top + 24, rect.right,
                   rect.top + std::clamp(summaryHeight * 3 / 4, 44, 54)};
    DrawTextInRect(memoryDc, std::get<1>(summary[i]), valueRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SetTextColor(memoryDc, kWidgetMuted);
    SelectObject(memoryDc, labelFont);
    RECT costRect{rect.left, rect.top + 52, rect.right, rect.bottom - 4};
    DrawTextInRect(memoryDc, std::get<2>(summary[i]), costRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(memoryDc, previousFont);
  }

  const int plugHeight = std::clamp(contentHeight / 7, 26, 38);
  RECT chart{content.left, content.top + 30 + summaryHeight + 14,
             content.right, content.bottom - plugHeight - 8};
  if (chart.bottom > chart.top + 20 && !nativeDashboard_.octopusHistory.empty()) {
    double maximum = 1.0;
    for (const auto& item : nativeDashboard_.octopusHistory) {
      if (std::isfinite(item.value)) maximum = std::max(maximum, item.value);
    }
    const int count = static_cast<int>(nativeDashboard_.octopusHistory.size());
    const int step = std::max(1, static_cast<int>(chart.right - chart.left) / std::max(1, count));
    const int barWidth = std::max(2, step * 7 / 10);
    SetTextColor(memoryDc, kWidgetMuted);
    HFONT chartFont = CreateUiFont(8, FW_NORMAL);
    previousFont = SelectObject(memoryDc, chartFont);
    for (int i = 0; i < count; ++i) {
      const double value = std::isfinite(nativeDashboard_.octopusHistory[i].value)
          ? nativeDashboard_.octopusHistory[i].value : 0.0;
      const int barHeight = static_cast<int>((chart.bottom - chart.top - 18) * value / maximum);
      const int x = chart.left + i * step + (step - barWidth) / 2;
      RECT barRect{x, chart.bottom - 18 - barHeight, x + barWidth, chart.bottom - 18};
      DrawWidgetPill(memoryDc, barRect, kWidgetOrange);
      RECT valueRect{x - 6, barRect.top - 13, x + barWidth + 6, barRect.top};
      DrawTextInRect(memoryDc, NumberOrDash(value, value >= 10 ? 0 : 1), valueRect,
                     DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    SelectObject(memoryDc, previousFont);
    DeleteObject(chartFont);
  }

  const int plugTop = std::max(static_cast<int>(content.bottom) - plugHeight,
                               static_cast<int>(chart.bottom) + 6);
  HFONT plugFont = CreateUiFont(std::clamp(plugHeight / 3, 9, 11), FW_NORMAL);
  previousFont = SelectObject(memoryDc, plugFont);
  SetTextColor(memoryDc, kWidgetMuted);
  RECT plugRect{content.left, plugTop, content.right, content.bottom};
  std::wstring plugs = L"Plug Mini情報なし";
  if (!nativeDashboard_.switchBotDevices.empty()) {
    plugs.clear();
    const size_t count = std::min<size_t>(2, nativeDashboard_.switchBotDevices.size());
    for (size_t i = 0; i < count; ++i) {
      if (i) plugs += L"  ";
      plugs += nativeDashboard_.switchBotDevices[i].name + L": " + nativeDashboard_.switchBotDevices[i].state;
    }
  }
  DrawTextInRect(memoryDc, plugs, plugRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
  SelectObject(memoryDc, previousFont);
  DeleteObject(plugFont);

  DeleteObject(labelFont);
  DeleteObject(valueFont);

  BitBlt(dc, 0, 0, bounds.right, bounds.bottom, memoryDc, 0, 0, SRCCOPY);
  SelectObject(memoryDc, previousBitmap);
  DeleteDC(memoryDc);
  EndPaint(hwnd, &paint);
}

void Renderer::PaintNativeStationhead(HWND hwnd) {
  PAINTSTRUCT paint{};
  HDC dc = BeginPaint(hwnd, &paint);
  if (!dc) return;

  RECT bounds{};
  GetClientRect(hwnd, &bounds);
  HDC memoryDc = CreateCompatibleDC(dc);
  HBITMAP bitmap = NativePanelBackBuffer(hwnd, dc, std::max(1L, bounds.right), std::max(1L, bounds.bottom));
  HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);
  FillWidgetBackground(memoryDc, bounds);
  SetBkMode(memoryDc, TRANSPARENT);
  const RECT content = DrawWidgetSurface(memoryDc, bounds);

  DrawWidgetHeader(memoryDc, L"Spotify WebView2", L"", content);

  HFONT labelFont = CreateUiFont(11, FW_NORMAL);
  HFONT titleFont = CreateUiFont(18, FW_SEMIBOLD);
  HFONT artistFont = CreateUiFont(12, FW_NORMAL);
  HFONT buttonFont = CreateUiFont(12, FW_SEMIBOLD);
  HGDIOBJ previousFont = nullptr;
  const StationheadButtonRects stationButtons = StationheadButtonsFromBounds(content);
  const int contentHeight = std::max(1L, content.bottom - content.top);
  const int rowGap = 18;
  const int rowTop = content.top + 30;
  const int rowHeight = std::max(56, (contentHeight - 30 - rowGap) / 2);
  const int contentWidth = std::max(1L, content.right - content.left);
  const bool compactRows = contentWidth < 300;

  const auto drawRow = [&](int row, const std::wstring& label, bool muted,
                           const NativePlaybackRender& playback,
                           const std::wstring& fallbackTitle, const std::wstring& fallbackArtist,
                           const std::wstring& detail) {
    const int top = rowTop + row * (rowHeight + rowGap);
    RECT rowRect{content.left, top, content.right, std::min<LONG>(top + rowHeight, content.bottom)};

    const int artSize = std::clamp(static_cast<int>(rowRect.bottom - rowRect.top) - 20, 42, 60);
    RECT art{rowRect.left + 10, rowRect.top + 10, rowRect.left + 10 + artSize,
             rowRect.top + 10 + artSize};
    DrawWidgetCard(memoryDc, art, kWidgetSurfaceAlt);
    if (playback.hasTrack) {
      DrawPremultipliedBitmap(memoryDc,
                              NativeArtworkBitmap(playback.track.artwork, art.right - art.left,
                                                  art.bottom - art.top),
                              art);
    }

    const std::wstring title = playback.hasTrack ? playback.track.title : fallbackTitle;
    const std::wstring artist = playback.hasTrack ? playback.track.artist : fallbackArtist;
    const bool withProgress = playback.hasTrack && playback.track.durationMs > 0;
    RECT button = row == 0 ? stationButtons.primaryAudio : stationButtons.secondaryAudio;
    if (compactRows) {
      button = RECT{rowRect.right - 88, rowRect.bottom - 34, rowRect.right - 10, rowRect.bottom - 8};
    }
    const int textRight = compactRows ? rowRect.right - 10 : std::max(art.right + 28, button.left - 8);

    SetTextColor(memoryDc, kWidgetMuted);
    previousFont = SelectObject(memoryDc, labelFont);
    RECT labelRect{art.right + 12, rowRect.top + 8, textRight, rowRect.top + 25};
    DrawTextInRect(memoryDc, label, labelRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    if (withProgress) {
      DrawTextInRect(memoryDc,
                     TrackTimeText(playback.progressMs) + L" / " +
                         TrackTimeText(playback.track.durationMs),
                     labelRect, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }

    SetTextColor(memoryDc, kWidgetText);
    SelectObject(memoryDc, titleFont);
    RECT titleRect{art.right + 12, rowRect.top + 27, textRight, rowRect.top + 52};
    DrawTextInRect(memoryDc, title.empty() ? L"--" : title, titleRect,
                   DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

    SetTextColor(memoryDc, kWidgetMuted);
    SelectObject(memoryDc, artistFont);
    RECT artistRect{art.right + 12, rowRect.top + 54, textRight,
                    compactRows ? button.top - 4 : withProgress ? rowRect.bottom - 18 : rowRect.bottom - 8};
    DrawTextInRect(memoryDc, artist.empty() ? detail : artist, artistRect,
                   DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

    if (withProgress) {
      RECT barRect{art.right + 12, compactRows ? button.top - 8 : rowRect.bottom - 14,
                   compactRows ? button.left - 8 : textRight,
                   compactRows ? button.top - 4 : rowRect.bottom - 10};
      DrawWidgetPill(memoryDc, barRect, kWidgetTrack);
      const double ratio = std::clamp(
          static_cast<double>(playback.progressMs) / static_cast<double>(playback.track.durationMs),
          0.0, 1.0);
      RECT fillRect = barRect;
      fillRect.right = barRect.left +
          static_cast<int>((barRect.right - barRect.left) * ratio);
      if (fillRect.right > fillRect.left) {
        DrawWidgetPill(memoryDc, fillRect, kWidgetGreen);
      }
    }

    DrawWidgetCard(memoryDc, button, muted ? kWidgetDangerSurface : kWidgetSuccessSurface);
    SetTextColor(memoryDc, muted ? kWidgetDanger : kWidgetGreen);
    SelectObject(memoryDc, buttonFont);
    DrawTextInRect(memoryDc, muted ? L"音声OFF" : L"音声ON", button,
                   DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  };

  const int64_t nowMs = UnixMillis();
  const NativePlaybackRender playbackA = ResolveNativePlayback(0, nowMs);
  const NativePlaybackRender playbackB = ResolveNativePlayback(1, nowMs);

  const std::wstring detail = nativeStationhead_.loginRequired ? L"ログイン待ち"
      : nativeStationhead_.processFailed ? L"WebView再起動待ち"
      : nativeStationhead_.audioPlaying ? L"再生中"
      : nativeStationhead_.created ? L"接続中"
      : L"起動待ち";
  drawRow(0, L"StationheadウインドウA", nativeStationhead_.audioMuted, playbackA,
          nativeStationhead_.trackTitle, nativeStationhead_.trackArtist, detail);
  drawRow(1, L"StationheadウインドウB", nativeStationhead_.secondaryAudioMuted, playbackB,
          L"Buddy46", L"",
          playbackB.available && !playbackB.hasTrack ? L"次の曲を待機中"
              : nativeStationhead_.secondaryAudioMuted ? L"音声OFF" : L"音声ON");

  SelectObject(memoryDc, previousFont);
  DeleteObject(labelFont);
  DeleteObject(titleFont);
  DeleteObject(artistFont);
  DeleteObject(buttonFont);

  BitBlt(dc, 0, 0, bounds.right, bounds.bottom, memoryDc, 0, 0, SRCCOPY);
  SelectObject(memoryDc, previousBitmap);
  DeleteDC(memoryDc);
  EndPaint(hwnd, &paint);
}

void Renderer::PaintNativeRadar(HWND hwnd) {
  PAINTSTRUCT paint{};
  HDC dc = BeginPaint(hwnd, &paint);
  if (!dc) return;

  RECT bounds{};
  GetClientRect(hwnd, &bounds);
  HDC memoryDc = CreateCompatibleDC(dc);
  HBITMAP bitmap = NativePanelBackBuffer(hwnd, dc, std::max(1L, bounds.right), std::max(1L, bounds.bottom));
  HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);
  FillWidgetBackground(memoryDc, bounds);
  SetBkMode(memoryDc, TRANSPARENT);
  const RECT content = DrawWidgetSurface(memoryDc, bounds);

  {
    std::lock_guard lock(radarFrameMutex_);
    DrawWidgetHeader(memoryDc, L"リアルタイム雨雲",
                     radarTimeText_.empty() ? L"--:--" : radarTimeText_, content);
    RECT stage{content.left, content.top + 26, content.right, content.bottom};
    DrawWidgetCard(memoryDc, stage, kWidgetStage);
    stage = NormalizeInsetRect(stage, 6, 6, 6, 6);
    const int stageWidth = std::max(1L, stage.right - stage.left);
    const int stageHeight = std::max(1L, stage.bottom - stage.top);
    if (radarFrameBitmap_) {
      const double scale = std::min(
          static_cast<double>(stageWidth) / kRadarCanvasWidth,
          static_cast<double>(stageHeight) / kRadarCanvasHeight);
      const int drawWidth = std::max(1, static_cast<int>(kRadarCanvasWidth * scale));
      const int drawHeight = std::max(1, static_cast<int>(kRadarCanvasHeight * scale));
      const int drawLeft = stage.left + (stageWidth - drawWidth) / 2;
      const int drawTop = stage.top + (stageHeight - drawHeight) / 2;
      HDC frameDc = CreateCompatibleDC(memoryDc);
      if (frameDc) {
        HGDIOBJ previousFrame = SelectObject(frameDc, radarFrameBitmap_);
        SetStretchBltMode(memoryDc, HALFTONE);
        SetBrushOrgEx(memoryDc, 0, 0, nullptr);
        StretchBlt(memoryDc, drawLeft, drawTop, drawWidth, drawHeight, frameDc, 0, 0,
                   kRadarCanvasWidth, kRadarCanvasHeight, SRCCOPY);
        SelectObject(frameDc, previousFrame);
        DeleteDC(frameDc);
      }
    } else {
      SetTextColor(memoryDc, kWidgetMuted);
      DrawTextInRect(memoryDc, L"待機中", stage,
                     DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
  }

  BitBlt(dc, 0, 0, bounds.right, bounds.bottom, memoryDc, 0, 0, SRCCOPY);
  SelectObject(memoryDc, previousBitmap);
  DeleteDC(memoryDc);
  EndPaint(hwnd, &paint);
}

HBITMAP Renderer::NativeArtworkBitmap(const std::wstring& url, int width, int height) {
  if (url.empty() || width <= 0 || height <= 0) return nullptr;
  static constexpr wchar_t kDataHostPrefix[] = L"https://data.homepanel/";
  if (url.rfind(kDataHostPrefix, 0) != 0) return nullptr;

  std::wostringstream keyStream;
  keyStream << url << L'#' << width << L'x' << height;
  const std::wstring key = keyStream.str();
  auto found = nativeArtworkBitmaps_.find(key);
  if (found != nativeArtworkBitmaps_.end()) {
    found->second.lastUsed = ++nativeArtworkUseCounter_;
    return found->second.bitmap;
  }

  std::wstring relative = url.substr(std::size(kDataHostPrefix) - 1);
  if (relative.empty() || relative.find(L"..") != std::wstring::npos) return nullptr;
  for (auto& character : relative) {
    if (character == L'/') character = L'\\';
  }

  HBITMAP bitmap = DecodeImageFileToBitmap(dataDir_ / relative, width, height);
  if (!bitmap) return nullptr;
  if (nativeArtworkBitmaps_.size() >= 48) {
    auto oldest = nativeArtworkBitmaps_.begin();
    for (auto item = nativeArtworkBitmaps_.begin(); item != nativeArtworkBitmaps_.end(); ++item) {
      if (item->second.lastUsed < oldest->second.lastUsed) oldest = item;
    }
    if (oldest->second.bitmap) DeleteObject(oldest->second.bitmap);
    nativeArtworkBitmaps_.erase(oldest);
  }
  nativeArtworkBitmaps_[key] = ArtworkBitmapCacheEntry{bitmap, ++nativeArtworkUseCounter_};
  return bitmap;
}
}  // namespace hp
