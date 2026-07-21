#include "web_renderer.h"
#include "wic_image.h"

namespace hp {
namespace {
constexpr int kNativeSideId = 101;
constexpr int kNativeMainId = 102;
constexpr int kNativeRadarId = 108;

constexpr COLORREF kWidgetBackground = kNativeDashboardBackground;
constexpr COLORREF kWidgetSurface = RGB(20, 26, 36);
constexpr COLORREF kWidgetSurfaceAlt = RGB(31, 39, 52);
constexpr COLORREF kWidgetOutline = RGB(43, 54, 69);
constexpr COLORREF kWidgetText = RGB(238, 242, 248);
constexpr COLORREF kWidgetMuted = RGB(159, 171, 189);
constexpr COLORREF kWidgetSubtle = RGB(110, 122, 141);

constexpr COLORREF kWidgetBlueMuted = RGB(100, 176, 255);
constexpr COLORREF kWidgetGreen = RGB(48, 209, 88);
constexpr COLORREF kWidgetOrange = RGB(255, 159, 10);
constexpr COLORREF kWidgetCyan = RGB(50, 173, 230);
constexpr COLORREF kWidgetBlack = RGB(0, 0, 0);
constexpr COLORREF kWidgetTrack = RGB(42, 52, 66);
constexpr COLORREF kWidgetWarning = RGB(255, 214, 10);
constexpr COLORREF kWidgetDanger = RGB(255, 69, 58);
constexpr COLORREF kWidgetDangerSurface = RGB(52, 34, 37);
constexpr COLORREF kWidgetSuccessSurface = RGB(26, 52, 38);

HBRUSH CachedSolidBrush(COLORREF color) {
  static std::map<COLORREF, HBRUSH> cache;
  auto [entry, inserted] = cache.try_emplace(color, nullptr);
  if (inserted) entry->second = CreateSolidBrush(color);
  return entry->second;
}

HPEN CachedSolidPen(int width, COLORREF color) {
  static std::map<std::pair<int, COLORREF>, HPEN> cache;
  const auto key = std::make_pair(width, color);
  auto [entry, inserted] = cache.try_emplace(key, nullptr);
  if (inserted) entry->second = CreatePen(PS_SOLID, width, color);
  return entry->second;
}

struct CachedSourceMemoryDc {
  HDC value = nullptr;
  ~CachedSourceMemoryDc() {
    if (value) DeleteDC(value);
  }
};

HDC SourceMemoryDc(HDC compatibleDc) {
  thread_local CachedSourceMemoryDc cached;
  if (!cached.value) cached.value = CreateCompatibleDC(compatibleDc);
  return cached.value;
}

void PlaceNativeWindow(HWND hwnd, const RECT& rect, bool visible) {
  SetWindowPos(hwnd, HWND_TOP, rect.left, rect.top,
               std::max(1L, rect.right - rect.left),
               std::max(1L, rect.bottom - rect.top),
               SWP_NOACTIVATE | SWP_NOSENDCHANGING |
                   (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

int SpanX(const RECT& rect, int permille) {
  return static_cast<int>((rect.right - rect.left) * permille / 1000);
}
int SpanY(const RECT& rect, int permille) {
  return static_cast<int>((rect.bottom - rect.top) * permille / 1000);
}

const wchar_t* DashboardFontFace() {
  static const wchar_t* face = [] {
    LOGFONTW probe{};
    probe.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(probe.lfFaceName, L"BIZ UDPGothic");
    bool found = false;
    HDC dc = GetDC(nullptr);
    if (dc) {
      EnumFontFamiliesExW(dc, &probe,
          [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM param) -> int {
            *reinterpret_cast<bool*>(param) = true;
            return 0;
          }, reinterpret_cast<LPARAM>(&found), 0);
      ReleaseDC(nullptr, dc);
    }
    return found ? L"BIZ UDPGothic" : L"Yu Gothic UI";
  }();
  return face;
}

HFONT CreateUiFont(int height, int weight) {
  return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, DashboardFontFace());
}

HFONT CachedUiFont(int height, int weight) {
  static std::map<std::pair<int, int>, HFONT> cache;
  const auto key = std::make_pair(height, weight);
  auto [entry, inserted] = cache.try_emplace(key, nullptr);
  if (inserted) entry->second = CreateUiFont(height, weight);
  return entry->second;
}

const std::wstring& TrackTimeText(int64_t milliseconds) {
  const int64_t seconds = std::max<int64_t>(0, milliseconds / 1000);
  struct CacheEntry {
    int64_t seconds = -1;
    std::wstring text;
  };
  thread_local std::array<CacheEntry, 4> cache{};
  thread_local size_t replacement = 0;
  for (const CacheEntry& entry : cache) {
    if (entry.seconds == seconds) return entry.text;
  }
  CacheEntry& entry = cache[replacement++ % cache.size()];
  wchar_t text[32]{};
  swprintf_s(text, L"%lld:%02lld", seconds / 60, seconds % 60);
  entry.seconds = seconds;
  entry.text = text;
  return entry.text;
}

int RecentStationheadPlayIncrease(const std::vector<StationheadPlayHistorySample>& history) {
  constexpr int64_t kRecentWindowMs = 60LL * 60 * 1000;
  if (history.size() < 2) return -1;

  const auto& latest = history.back();
  const int64_t target = latest.timestamp - kRecentWindowMs;
  const auto baseline = std::upper_bound(
      history.begin(), history.end(), target,
      [](int64_t timestamp, const StationheadPlayHistorySample& sample) {
        return timestamp < sample.timestamp;
      });
  if (baseline == history.begin()) return -1;

  const int delta = latest.value - std::prev(baseline)->value;
  // The daily counter can reset at midnight; in that case the current value
  // is the number accumulated since the reset rather than a negative delta.
  return delta >= 0 ? delta : latest.value;
}

bool IsStationheadFallbackUrl(const std::wstring& url, const std::wstring& fallbackUrl) {
  return !url.empty() && !fallbackUrl.empty() && _wcsicmp(url.c_str(), fallbackUrl.c_str()) == 0;
}

void DrawPremultipliedBitmap(HDC dc, HBITMAP bitmap, const RECT& target) {
  if (!bitmap) return;
  const int width = target.right - target.left;
  const int height = target.bottom - target.top;
  if (width <= 0 || height <= 0) return;
  HDC sourceDc = SourceMemoryDc(dc);
  if (!sourceDc) return;
  HGDIOBJ previousBitmap = SelectObject(sourceDc, bitmap);
  const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  AlphaBlend(dc, target.left, target.top, width, height, sourceDc, 0, 0, width, height, blend);
  SelectObject(sourceDc, previousBitmap);
}

const std::wstring& DateText(const SYSTEMTIME& now) {
  struct DateTextCache {
    WORD year = 0;
    WORD month = 0;
    WORD day = 0;
    std::wstring text;
  };
  static DateTextCache cache;
  if (cache.year == now.wYear && cache.month == now.wMonth && cache.day == now.wDay) {
    return cache.text;
  }
  static constexpr const wchar_t* kWeekdays[] = {L"日", L"月", L"火", L"水", L"木", L"金", L"土"};
  wchar_t text[64]{};
  swprintf_s(text, L"%04u年%u月%u日 (%s)", now.wYear, now.wMonth, now.wDay,
             kWeekdays[now.wDayOfWeek % 7]);
  cache.year = now.wYear;
  cache.month = now.wMonth;
  cache.day = now.wDay;
  cache.text = text;
  return cache.text;
}

const std::wstring& TimeText(const SYSTEMTIME& now) {
  const int secondOfDay = now.wHour * 3600 + now.wMinute * 60 + now.wSecond;
  thread_local int cachedSecond = -1;
  thread_local std::wstring cachedText;
  if (cachedSecond == secondOfDay) return cachedText;
  wchar_t text[16]{};
  swprintf_s(text, L"%02u:%02u:%02u", now.wHour, now.wMinute, now.wSecond);
  cachedSecond = secondOfDay;
  cachedText = text;
  return cachedText;
}

void DrawTextInRect(HDC dc, const wchar_t* text, RECT rect, int format) {
  DrawTextW(dc, text ? text : L"", -1, &rect, format | DT_NOPREFIX);
}

void DrawTextInRect(HDC dc, const std::wstring& text, RECT rect, int format) {
  DrawTextW(dc, text.c_str(), -1, &rect, format | DT_NOPREFIX);
}

void FillWidgetBackground(HDC dc, const RECT& bounds) {
  FillRect(dc, &bounds, CachedSolidBrush(kWidgetBackground));
}

void AlphaBlendSolidColor(HDC dc, const RECT& rect, COLORREF color, BYTE alpha);

HBITMAP SolidColorBitmap(COLORREF color) {
  static std::map<COLORREF, HBITMAP> cache;
  auto [entry, inserted] = cache.try_emplace(color, nullptr);
  if (!inserted) return entry->second;

  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = 1;
  info.bmiHeader.biHeight = -1;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* pixel = nullptr;
  HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixel, nullptr, 0);
  if (bitmap && pixel) {
    *static_cast<uint32_t*>(pixel) =
        (GetRValue(color) << 16) | (GetGValue(color) << 8) | GetBValue(color);
  }
  entry->second = bitmap;
  return bitmap;
}

void FillRoundRectTranslucent(HDC dc, const RECT& rect, COLORREF color, int radius, BYTE alpha) {
  HRGN clip = CreateRoundRectRgn(rect.left, rect.top, rect.right + 1, rect.bottom + 1, radius, radius);
  SelectClipRgn(dc, clip);
  AlphaBlendSolidColor(dc, rect, color, alpha);
  SelectClipRgn(dc, nullptr);
  DeleteObject(clip);
}

void DrawWidgetCard(HDC dc, const RECT& rect, COLORREF color = kWidgetSurface,
                    int radius = 14, BYTE alpha = 255) {
  if (rect.right <= rect.left || rect.bottom <= rect.top) return;
  if (alpha < 255) {
    FillRoundRectTranslucent(dc, rect, color, radius, alpha);
    return;
  }
  HGDIOBJ previousPen = SelectObject(dc, GetStockObject(NULL_PEN));
  HGDIOBJ previousBrush = SelectObject(dc, CachedSolidBrush(color));
  RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
  SelectObject(dc, previousBrush);
  SelectObject(dc, previousPen);
}

void DrawWidgetPill(HDC dc, const RECT& rect, COLORREF color, BYTE alpha = 255) {
  if (rect.right <= rect.left || rect.bottom <= rect.top) return;
  const int radius = std::max(2L, rect.bottom - rect.top);
  if (alpha < 255) {
    FillRoundRectTranslucent(dc, rect, color, radius, alpha);
    return;
  }
  HGDIOBJ previousPen = SelectObject(dc, GetStockObject(NULL_PEN));
  HGDIOBJ previousBrush = SelectObject(dc, CachedSolidBrush(color));
  RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
  SelectObject(dc, previousBrush);
  SelectObject(dc, previousPen);
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

bool IsWeatherNightHour(int hour) {
  return hour < 6 || hour >= 18;
}

template <typename ValueOf>
void DrawHistoryLine(HDC dc, const std::vector<AirHistorySample>& samples, const RECT& plot,
                     int64_t cutoff, int64_t spanMs, double minValue, double maxValue,
                     COLORREF color, int width, ValueOf&& valueOf) {
  if (samples.empty() || maxValue <= minValue) return;
  HGDIOBJ previousPen = SelectObject(dc, CachedSolidPen(width, color));
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
}

void AlphaBlendSolidColor(HDC dc, const RECT& rect, COLORREF color, BYTE alpha) {
  if (alpha == 0 || rect.right <= rect.left || rect.bottom <= rect.top) return;
  HBITMAP colorBitmap = SolidColorBitmap(color);
  if (!colorBitmap) return;
  HDC colorDc = SourceMemoryDc(dc);
  if (!colorDc) return;
  HGDIOBJ previousBitmap = SelectObject(colorDc, colorBitmap);
  const BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, 0};
  AlphaBlend(dc, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
             colorDc, 0, 0, 1, 1, blend);
  SelectObject(colorDc, previousBitmap);
}

int CardRadius(const RECT& card) {
  const int width = std::max(1L, card.right - card.left);
  const int height = std::max(1L, card.bottom - card.top);
  return std::clamp(std::min(width, height) * 16 / 100, 10, 28);
}

RECT CardContentRect(const RECT& card) {
  const int width = std::max(1L, card.right - card.left);
  const int height = std::max(1L, card.bottom - card.top);
  const int pad = std::clamp(std::min(width, height) * 10 / 100, 8, 26);
  return NormalizeInsetRect(card, pad, pad, pad, pad);
}

int CardHeaderHeight(const RECT& content) {
  return std::clamp(SpanY(content, 140), 16, 34);
}

RECT CardBodyRect(const RECT& content) {
  RECT body = content;
  body.top += CardHeaderHeight(content) + std::max(4, SpanY(content, 40));
  if (body.bottom <= body.top) body.bottom = body.top + 1;
  return body;
}

void DrawCardOutline(HDC dc, const RECT& rect, int radius) {
  if (rect.right <= rect.left || rect.bottom <= rect.top) return;
  HGDIOBJ previousPen = SelectObject(dc, CachedSolidPen(1, kWidgetOutline));
  HGDIOBJ previousBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
  RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
  SelectObject(dc, previousBrush);
  SelectObject(dc, previousPen);
}

void DrawSectionCard(HDC dc, const RECT& rect) {
  const int radius = CardRadius(rect);
  DrawWidgetCard(dc, rect, kWidgetSurface, radius);
  DrawCardOutline(dc, rect, radius);
}

struct SidebarSections {
  RECT clock{};
  RECT weather{};
  RECT energy{};
  RECT controls{};
};

SidebarSections SplitSidebarSections(const RECT& client) {
  const int height = std::max(1L, client.bottom - client.top);
  const int gap = std::max(6, height * 18 / 1000);
  const int radarHeight = height * 600 / 1000;
  const int mainGap = std::max(6, height * 18 / 1000);
  const int mainHeight = std::max(1, height - radarHeight - mainGap);
  const int targetControlsHeight = std::max(1, mainHeight * 270 / 1000);
  const int availableForCards = std::max(3, height - gap * 3);
  const int controlsHeight = std::clamp(targetControlsHeight, 1, std::max(1, availableForCards - 2));
  const int cardHeight = std::max(1, (availableForCards - controlsHeight) / 3);
  SidebarSections sections;
  int top = client.top;
  sections.clock = RECT{client.left, top, client.right, top + cardHeight};
  top = sections.clock.bottom + gap;
  sections.weather = RECT{client.left, top, client.right, top + cardHeight};
  top = sections.weather.bottom + gap;
  sections.energy = RECT{client.left, top, client.right, top + cardHeight};
  top = sections.energy.bottom + gap;
  sections.controls = RECT{client.left, top, client.right, top + controlsHeight};
  return sections;
}

struct MainSections {
  RECT music{};
  RECT air{};
  RECT news{};
};

MainSections SplitMainSections(const RECT& client) {
  const int width = std::max(1L, client.right - client.left);
  const int height = std::max(1L, client.bottom - client.top);
  const int gapX = std::max(8, width * 16 / 1000);
  const int gapY = std::max(6, height * 45 / 1000);
  const int newsHeight = height * 270 / 1000;
  const int musicWidth = width * 480 / 1000;
  MainSections sections;
  sections.news = RECT{client.left, client.bottom - newsHeight, client.right, client.bottom};
  const LONG rowBottom = std::max(client.top + 1, sections.news.top - gapY);
  sections.music = RECT{client.left, client.top, client.left + musicWidth, rowBottom};
  sections.air = RECT{sections.music.right + gapX, client.top, client.right, rowBottom};
  if (sections.air.right <= sections.air.left) sections.air.right = sections.air.left + 1;
  return sections;
}

struct MusicRowRects {
  RECT row{};
  RECT artwork{};
  RECT button{};
  RECT muteButton{};
};

MusicRowRects MusicRowFromBody(const RECT& body) {
  const int rowHeight = std::max(1, static_cast<int>(body.bottom - body.top));
  MusicRowRects rects;
  rects.row = body;
  const int pad = std::max(2, rowHeight * 8 / 100);
  const int artSize = std::max(1, rowHeight - pad * 2);
  rects.artwork = RECT{rects.row.left, rects.row.top + pad, rects.row.left + artSize,
                       rects.row.top + pad + artSize};
  const int buttonGap = std::max(3, rowHeight * 4 / 100);
  const int buttonHeight = std::min(std::clamp(rowHeight * 28 / 100, 20, 44),
                                    std::max(1, (rowHeight - buttonGap) / 2));
  const int buttonWidth = std::clamp(SpanX(body, 170), 64, 140);
  const int retiredFooterHeight = std::clamp(rowHeight * 14 / 100, 16, 30);
  const int clusterHeight = buttonHeight * 2 + retiredFooterHeight + buttonGap * 2;
  const int clusterTop = rects.row.top + std::max(0, (rowHeight - clusterHeight) / 2);
  rects.button = RECT{rects.row.right - buttonWidth, clusterTop, rects.row.right,
                      clusterTop + buttonHeight};
  rects.muteButton = RECT{rects.row.right - buttonWidth, rects.button.bottom + buttonGap,
                          rects.row.right, rects.button.bottom + buttonGap + buttonHeight};
  return rects;
}

RECT MusicProgressRectFromBody(const RECT& body) {
  const MusicRowRects rects = MusicRowFromBody(body);
  const int rowHeight = std::max(1, static_cast<int>(rects.row.bottom - rects.row.top));
  const int pad = std::max(6, rowHeight * 10 / 100);
  RECT progress{rects.artwork.right + pad,
                rects.row.top + rowHeight * 64 / 100,
                rects.button.left - pad,
                rects.row.bottom - rowHeight * 2 / 100};
  if (progress.right <= progress.left) progress.right = progress.left + 1;
  if (progress.bottom <= progress.top) progress.bottom = progress.top + 1;
  return progress;
}

RECT ClockTimeRectFromCard(const RECT& card) {
  const RECT content = CardContentRect(card);
  const LONG dateBottom = content.top + SpanY(content, 320);
  RECT timeRect{content.left, dateBottom, content.right, content.bottom};
  if (timeRect.right <= timeRect.left) timeRect.right = timeRect.left + 1;
  if (timeRect.bottom <= timeRect.top) timeRect.bottom = timeRect.top + 1;
  return timeRect;
}

void StretchRadarInto(HDC destDc, const RECT& destRect, HBITMAP radarBitmap) {
  const int destWidth = static_cast<int>(destRect.right - destRect.left);
  const int destHeight = static_cast<int>(destRect.bottom - destRect.top);
  if (!radarBitmap || destWidth <= 0 || destHeight <= 0) return;
  HDC sourceDc = CreateCompatibleDC(destDc);
  if (!sourceDc) return;
  const double scale = std::max(static_cast<double>(destWidth) / kRadarCanvasWidth,
                                static_cast<double>(destHeight) / kRadarCanvasHeight);
  const int sourceWidth = std::clamp(static_cast<int>(std::lround(destWidth / scale)), 1, kRadarCanvasWidth);
  const int sourceHeight = std::clamp(static_cast<int>(std::lround(destHeight / scale)), 1, kRadarCanvasHeight);
  const int sourceLeft = (kRadarCanvasWidth - sourceWidth) / 2;
  const int sourceTop = (kRadarCanvasHeight - sourceHeight) / 2;
  HGDIOBJ previous = SelectObject(sourceDc, radarBitmap);
  SetStretchBltMode(destDc, HALFTONE);
  SetBrushOrgEx(destDc, 0, 0, nullptr);
  StretchBlt(destDc, destRect.left, destRect.top, destWidth, destHeight,
             sourceDc, sourceLeft, sourceTop, sourceWidth, sourceHeight, SRCCOPY);
  SelectObject(sourceDc, previous);
  DeleteDC(sourceDc);
}

struct CachedPanelMemoryDc {
  HDC value = nullptr;
  ~CachedPanelMemoryDc() {
    if (value) DeleteDC(value);
  }
};

HDC NativePanelMemoryDc(HDC compatibleDc) {
  thread_local CachedPanelMemoryDc cached;
  if (!cached.value) cached.value = CreateCompatibleDC(compatibleDc);
  return cached.value;
}

constexpr UINT_PTR kNativePanelTickTimer = 1;
constexpr UINT kNativePanelTickMs = 1'000;
}
