#include "web_renderer.h"
#include "wic_image.h"

namespace hp {
namespace {
constexpr int kNativeSideId = 101;
constexpr int kNativeMainId = 102;
constexpr int kNativeRadarId = 108;
constexpr size_t kNativeBitmapCacheLimit = 24;

constexpr COLORREF kWidgetBackground = kNativeDashboardBackground;
constexpr COLORREF kWidgetSurface = RGB(20, 26, 36);
constexpr COLORREF kWidgetSurfaceAlt = RGB(31, 39, 52);
constexpr COLORREF kWidgetOutline = RGB(43, 54, 69);
constexpr COLORREF kWidgetText = RGB(238, 242, 248);
constexpr COLORREF kWidgetMuted = RGB(159, 171, 189);
constexpr COLORREF kWidgetSubtle = RGB(110, 122, 141);

constexpr COLORREF kWidgetBlue = RGB(10, 132, 255);
constexpr COLORREF kWidgetBlueMuted = RGB(100, 176, 255);
constexpr COLORREF kWidgetGreen = RGB(48, 209, 88);
constexpr COLORREF kWidgetOrange = RGB(255, 159, 10);
constexpr COLORREF kWidgetCyan = RGB(50, 173, 230);
constexpr COLORREF kWidgetPurple = RGB(175, 82, 222);
constexpr COLORREF kWidgetBlack = RGB(0, 0, 0);
constexpr COLORREF kWidgetTrack = RGB(42, 52, 66);
constexpr COLORREF kWidgetWarning = RGB(255, 214, 10);
constexpr COLORREF kWidgetDanger = RGB(255, 69, 58);
constexpr COLORREF kWidgetDangerSurface = RGB(52, 34, 37);
constexpr COLORREF kWidgetSuccessSurface = RGB(26, 52, 38);

void PlaceNativeWindow(HWND hwnd, const RECT& rect, bool visible) {
  SetWindowPos(hwnd, HWND_TOP, rect.left, rect.top,
               std::max(1L, rect.right - rect.left),
               std::max(1L, rect.bottom - rect.top),
               SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : SWP_NOOWNERZORDER));
  ShowWindow(hwnd, visible ? SW_SHOWNA : SW_HIDE);
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
  HBRUSH fill = CreateSolidBrush(color);
  HGDIOBJ previousPen = SelectObject(dc, GetStockObject(NULL_PEN));
  HGDIOBJ previousBrush = SelectObject(dc, fill);
  RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
  SelectObject(dc, previousBrush);
  SelectObject(dc, previousPen);
  DeleteObject(fill);
}

void DrawWidgetPill(HDC dc, const RECT& rect, COLORREF color, BYTE alpha = 255) {
  if (rect.right <= rect.left || rect.bottom <= rect.top) return;
  const int radius = std::max(2L, rect.bottom - rect.top);
  if (alpha < 255) {
    FillRoundRectTranslucent(dc, rect, color, radius, alpha);
    return;
  }
  HBRUSH fill = CreateSolidBrush(color);
  HGDIOBJ previousPen = SelectObject(dc, GetStockObject(NULL_PEN));
  HGDIOBJ previousBrush = SelectObject(dc, fill);
  RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
  SelectObject(dc, previousBrush);
  SelectObject(dc, previousPen);
  DeleteObject(fill);
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

void AlphaBlendSolidColor(HDC dc, const RECT& rect, COLORREF color, BYTE alpha) {
  if (alpha == 0 || rect.right <= rect.left || rect.bottom <= rect.top) return;
  HBITMAP colorBitmap = SolidColorBitmap(color);
  if (!colorBitmap) return;
  HDC colorDc = CreateCompatibleDC(dc);
  if (!colorDc) return;
  HGDIOBJ previousBitmap = SelectObject(colorDc, colorBitmap);
  const BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, 0};
  AlphaBlend(dc, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
            colorDc, 0, 0, 1, 1, blend);
  SelectObject(colorDc, previousBitmap);
  DeleteDC(colorDc);
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
  HPEN pen = CreatePen(PS_SOLID, 1, kWidgetOutline);
  HGDIOBJ previousPen = SelectObject(dc, pen);
  HGDIOBJ previousBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
  RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
  SelectObject(dc, previousBrush);
  SelectObject(dc, previousPen);
  DeleteObject(pen);
}

void DrawSectionCard(HDC dc, const RECT& rect) {
  const int radius = CardRadius(rect);
  DrawWidgetCard(dc, rect, kWidgetSurface, radius);
  DrawCardOutline(dc, rect, radius);
}

void DrawHeaderStatus(HDC dc, const RECT& header, const PanelDataStatus& status) {
  const wchar_t* text = nullptr;
  COLORREF color = kWidgetSubtle;
  switch (status.state) {
    case PanelDataState::Waiting:
      text = L"取得待ち";
      color = kWidgetSubtle;
      break;
    case PanelDataState::Stale:
      text = L"更新停滞";
      color = kWidgetWarning;
      break;
    case PanelDataState::Error:
      text = L"取得エラー";
      color = kWidgetDanger;
      break;
    case PanelDataState::Ok:
    default:
      return;
  }
  SetTextColor(dc, color);
  DrawTextInRect(dc, text, header, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
}

struct SidebarSections {
  RECT clock{};
  RECT air{};
  RECT weather{};
  RECT controls{};
};

SidebarSections SplitSidebarSections(const RECT& client) {
  const int height = std::max(1L, client.bottom - client.top);
  const int gap = std::max(6, height * 18 / 1000);
  const int clockHeight = height * 150 / 1000;
  const int airHeight = height * 345 / 1000;
  const int weatherHeight = height * 305 / 1000;
  SidebarSections sections;
  int top = client.top;
  sections.clock = RECT{client.left, top, client.right, top + clockHeight};
  top = sections.clock.bottom + gap;
  sections.air = RECT{client.left, top, client.right, top + airHeight};
  top = sections.air.bottom + gap;
  sections.weather = RECT{client.left, top, client.right, top + weatherHeight};
  top = sections.weather.bottom + gap;
  sections.controls = RECT{client.left, std::min(static_cast<LONG>(top), client.bottom - 1),
                           client.right, client.bottom};
  return sections;
}

struct MainSections {
  RECT music{};
  RECT energy{};
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
  sections.energy = RECT{sections.music.right + gapX, client.top, client.right, rowBottom};
  if (sections.energy.right <= sections.energy.left) {
    sections.energy.right = sections.energy.left + 1;
  }
  return sections;
}

struct ControlsRects {
  RECT update{};
  RECT restart{};
  RECT status{};
};

ControlsRects ControlsFromContent(const RECT& content) {
  const int height = std::max(1L, content.bottom - content.top);
  const int width = std::max(1L, content.right - content.left);
  const int gap = std::max(6, SpanX(content, 35));
  const int buttonHeight = std::clamp(height * 45 / 100, 28, 54);
  const int buttonWidth = std::max(1, (width - gap) / 2);
  ControlsRects rects;
  rects.update = RECT{content.left, content.top, content.left + buttonWidth,
                      content.top + buttonHeight};
  rects.restart = RECT{rects.update.right + gap, content.top, content.right,
                       content.top + buttonHeight};
  rects.status = RECT{content.left, rects.update.bottom + std::max(4, height * 8 / 100),
                      content.right, content.bottom};
  if (rects.status.bottom <= rects.status.top) rects.status.bottom = rects.status.top + 1;
  return rects;
