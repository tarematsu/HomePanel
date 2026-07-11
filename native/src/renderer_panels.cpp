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
  const int musicWidth = width * 535 / 1000;
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
}

struct MusicRowRects {
  RECT row{};
  RECT artwork{};
  RECT button{};
};

MusicRowRects MusicRowFromBody(const RECT& body, int row) {
  const int gap = std::max(4, SpanY(body, 60));
  const int rowHeight = std::max(1, (static_cast<int>(body.bottom - body.top) - gap) / 2);
  const int top = body.top + row * (rowHeight + gap);
  MusicRowRects rects;
  rects.row = RECT{body.left, top, body.right, top + rowHeight};
  const int pad = std::max(2, rowHeight * 8 / 100);
  const int artSize = std::max(1, rowHeight - pad * 2);
  rects.artwork = RECT{rects.row.left, rects.row.top + pad, rects.row.left + artSize,
                       rects.row.top + pad + artSize};
  const int buttonHeight = std::clamp(rowHeight * 34 / 100, 24, 44);
  const int buttonWidth = std::clamp(SpanX(body, 170), 64, 140);
  const int buttonTop = rects.row.top + (rowHeight - buttonHeight) / 2;
  rects.button = RECT{rects.row.right - buttonWidth, buttonTop, rects.row.right,
                      buttonTop + buttonHeight};
  return rects;
}

void StretchRadarInto(HDC destDc, const RECT& destRect, HBITMAP radarBitmap) {
  const int destWidth = static_cast<int>(destRect.right - destRect.left);
  const int destHeight = static_cast<int>(destRect.bottom - destRect.top);
  if (!radarBitmap || destWidth <= 0 || destHeight <= 0) return;
  HDC sourceDc = CreateCompatibleDC(destDc);
  if (!sourceDc) return;
  const double scale = std::max(static_cast<double>(destWidth) / kRadarCanvasWidth,
                                static_cast<double>(destHeight) / kRadarCanvasHeight);
  const int sourceWidth = std::clamp(
      static_cast<int>(std::lround(destWidth / scale)), 1, kRadarCanvasWidth);
  const int sourceHeight = std::clamp(
      static_cast<int>(std::lround(destHeight / scale)), 1, kRadarCanvasHeight);
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
}

const std::array<Renderer::NativePanelSlot, 3>& Renderer::NativePanelSlots() {
  static const std::array<NativePanelSlot, 3> slots{{
      {&Renderer::nativeRadarWindow_, &NativeDashboardLayout::radar,
       L"HomePanelNativeRadar", kNativeRadarId},
      {&Renderer::nativeSideWindow_, &NativeDashboardLayout::side,
       L"HomePanelNativeSide", kNativeSideId},
      {&Renderer::nativeMainWindow_, &NativeDashboardLayout::main,
       L"HomePanelNativeMain", kNativeMainId},
  }};
  return slots;
}

HFONT Renderer::TierFont(FontTier tier) const {
  const int clientHeight = std::max(1L, bounds_.bottom - bounds_.top);
  switch (tier) {
    case FontTier::Small:
      return CachedUiFont(std::clamp(clientHeight * 14 / 1000, 12, 24), FW_NORMAL);
    case FontTier::Medium:
      return CachedUiFont(std::clamp(clientHeight * 21 / 1000, 16, 36), FW_SEMIBOLD);
    case FontTier::Large:
    default:
      return CachedUiFont(std::clamp(clientHeight * 56 / 1000, 36, 112), FW_SEMIBOLD);
  }
}

Renderer::NativePanelPaintScope::NativePanelPaintScope(Renderer& renderer, HWND hwnd)
    : hwnd(hwnd), paintDc(BeginPaint(hwnd, &paint)) {
  if (!paintDc) return;
  GetClientRect(hwnd, &bounds);
  dirty = paint.rcPaint;
  if (IsRectEmpty(&dirty)) dirty = bounds;
  IntersectRect(&dirty, &dirty, &bounds);
  dc = CreateCompatibleDC(paintDc);
  HBITMAP bitmap = renderer.NativePanelBackBuffer(hwnd, paintDc, std::max(1L, bounds.right), std::max(1L, bounds.bottom));
  if (!dc || !bitmap) {
    if (dc) {
      DeleteDC(dc);
      dc = nullptr;
    }
    FillWidgetBackground(paintDc, bounds);
    return;
  }
  previousBitmap = SelectObject(dc, bitmap);
  HRGN dirtyClip = CreateRectRgnIndirect(&dirty);
  SelectClipRgn(dc, dirtyClip);
  DeleteObject(dirtyClip);
  FillWidgetBackground(dc, dirty);
  SetBkMode(dc, TRANSPARENT);
}

Renderer::NativePanelPaintScope::~NativePanelPaintScope() {
  if (!paintDc) return;
  if (dc) {
    BitBlt(paintDc, dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top,
           dc, dirty.left, dirty.top, SRCCOPY);
    SelectObject(dc, previousBitmap);
    DeleteDC(dc);
  }
  EndPaint(hwnd, &paint);
}

bool Renderer::EnsureNativeStaticWindows() {
  if (!window_ || !IsWindow(window_)) return false;
  static constexpr wchar_t kStaticClassName[] = L"HomePanelNativeStaticPanel";
  static std::once_flag classOnce;
  std::call_once(classOnce, [] {
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = Renderer::NativeWndProcThunk<&Renderer::HandleNativeStaticMessage>;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kStaticClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    RegisterClassW(&windowClass);
  });

  const NativeDashboardLayout layout = ComputeNativeDashboardLayout(bounds_);
  bool allCreated = true;
  bool createdAny = false;
  for (const NativePanelSlot& slot : NativePanelSlots()) {
    HWND& hwnd = this->*slot.window;
    if (hwnd && IsWindow(hwnd)) continue;
    const RECT rect = layout.*slot.rect;
    hwnd = CreateWindowExW(0, kStaticClassName, slot.title,
        WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE,
        rect.left, rect.top, std::max(1L, rect.right - rect.left),
        std::max(1L, rect.bottom - rect.top), window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(slot.id)),
        GetModuleHandleW(nullptr), this);
    const bool created = hwnd && IsWindow(hwnd);
    allCreated = allCreated && created;
    createdAny = createdAny || created;
  }
  if (createdAny) ApplyNativeStaticBounds();
  return allCreated;
}

void Renderer::ApplyNativeStaticBounds() {
  const NativeDashboardLayout layout = ComputeNativeDashboardLayout(bounds_);
  for (const NativePanelSlot& slot : NativePanelSlots()) {
    const HWND hwnd = this->*slot.window;
    if (!hwnd || !IsWindow(hwnd)) continue;
    PlaceNativeWindow(hwnd, layout.*slot.rect, nativeDashboardVisible_);
    InvalidateRect(hwnd, nullptr, FALSE);
  }
}

void Renderer::InvalidateAllNativePanels() {
  for (const NativePanelSlot& slot : NativePanelSlots()) {
    const HWND hwnd = this->*slot.window;
    if (hwnd && IsWindow(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
  }
}

void Renderer::InvalidatePanelSection(HWND window, PanelSection section) {
  if (!window || !IsWindow(window)) return;
  RECT client{};
  GetClientRect(window, &client);
  RECT rect = client;
  if (window == nativeSideWindow_) {
    const SidebarSections sections = SplitSidebarSections(client);
    switch (section) {
      case PanelSection::Clock: rect = sections.clock; break;
      case PanelSection::Air: rect = sections.air; break;
      case PanelSection::Weather: rect = sections.weather; break;
      case PanelSection::Controls: rect = sections.controls; break;
      default: break;
    }
  } else if (window == nativeMainWindow_) {
    const MainSections sections = SplitMainSections(client);
    switch (section) {
      case PanelSection::Music: rect = sections.music; break;
      case PanelSection::Energy: rect = sections.energy; break;
      case PanelSection::News: rect = sections.news; break;
      default: break;
    }
  }
  InvalidateRect(window, &rect, FALSE);
}

void Renderer::DestroyNativeStaticWindows() {
  for (const NativePanelSlot& slot : NativePanelSlots()) {
    const HWND hwnd = this->*slot.window;
    if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
  }
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
  for (const NativePanelSlot& slot : NativePanelSlots()) this->*slot.window = nullptr;
}

void Renderer::UpdateNativeStaticPanels(const RenderState& state) {
  const bool sensorsChanged = nativeSensors_ != state.sensors;
  const bool historyChanged = nativeAirHistory_ != state.airHistory;
  const bool stationheadChanged = nativeStationhead_ != state.stationhead;
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
  if (sensorsChanged || historyChanged) InvalidatePanelSection(nativeSideWindow_, PanelSection::Air);
  if (dashboardChanged) {
    InvalidatePanelSection(nativeSideWindow_, PanelSection::Weather);
    InvalidatePanelSection(nativeMainWindow_, PanelSection::Energy);
  }
  if (dashboardChanged || newsChanged) InvalidatePanelSection(nativeMainWindow_, PanelSection::News);
  if (controlsChanged) InvalidatePanelSection(nativeSideWindow_, PanelSection::Controls);
  if (stationheadChanged) InvalidatePanelSection(nativeMainWindow_, PanelSection::Music);
}

void Renderer::TickNativePanels(int64_t nowMs) {
  if (!nativeDashboardVisible_) return;
  if (nativeSideWindow_ && IsWindow(nativeSideWindow_) && IsWindowVisible(nativeSideWindow_)) {
    InvalidatePanelSection(nativeSideWindow_, PanelSection::Clock);
  }
  if (nativeMainWindow_ && IsWindow(nativeMainWindow_) && IsWindowVisible(nativeMainWindow_) &&
      NativePlaybackActive(nowMs)) {
    InvalidatePanelSection(nativeMainWindow_, PanelSection::Music);
  }
}

LRESULT Renderer::HandleNativeStaticMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_ERASEBKGND:
      return 1;
    case WM_LBUTTONUP: {
      const int id = GetDlgCtrlID(hwnd);
      RECT client{};
      GetClientRect(hwnd, &client);
      const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (id == kNativeSideId) {
        const SidebarSections sections = SplitSidebarSections(client);
        const ControlsRects controls = ControlsFromContent(CardContentRect(sections.controls));
        if (PtInRect(&controls.update, point)) QueueAction(UiAction::AppUpdate);
        else if (PtInRect(&controls.restart, point)) QueueAction(UiAction::Restart);
        return 0;
      }
      if (id == kNativeMainId) {
        const MainSections sections = SplitMainSections(client);
        const RECT body = CardBodyRect(CardContentRect(sections.music));
        for (int row = 0; row < 2; ++row) {
          const MusicRowRects rects = MusicRowFromBody(body, row);
          if (PtInRect(&rects.button, point)) {
            QueueAction(row == 0 ? UiAction::StationheadAudioToggleA
                                 : UiAction::StationheadAudioToggleB);
            break;
          }
        }
        return 0;
      }
      break;
    }
    case WM_PAINT:
      if (GetDlgCtrlID(hwnd) == kNativeSideId) PaintNativeSide(hwnd);
      else if (GetDlgCtrlID(hwnd) == kNativeMainId) PaintNativeMain(hwnd);
      else PaintNativeRadar(hwnd);
      return 0;
    case WM_NCDESTROY:
      ReleaseNativePanelBackBuffer(hwnd);
      for (const NativePanelSlot& slot : NativePanelSlots()) {
        if (this->*slot.window == hwnd) this->*slot.window = nullptr;
      }
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      break;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

void Renderer::PaintNativeSide(HWND hwnd) {
  NativePanelPaintScope scope(*this, hwnd);
  if (!scope.Valid()) return;
  const SidebarSections sections = SplitSidebarSections(scope.bounds);
  RECT overlap{};
  if (IntersectRect(&overlap, &scope.dirty, &sections.clock)) {
    DrawClockSection(scope.dc, sections.clock);
  }
  if (IntersectRect(&overlap, &scope.dirty, &sections.air)) {
    DrawAirSection(scope.dc, sections.air);
  }
  if (IntersectRect(&overlap, &scope.dirty, &sections.weather)) {
    DrawWeatherSection(scope.dc, sections.weather);
  }
  if (IntersectRect(&overlap, &scope.dirty, &sections.controls)) {
    DrawControlsSection(scope.dc, sections.controls);
  }
}

void Renderer::PaintNativeMain(HWND hwnd) {
  NativePanelPaintScope scope(*this, hwnd);
  if (!scope.Valid()) return;
  const MainSections sections = SplitMainSections(scope.bounds);
  RECT overlap{};
  if (IntersectRect(&overlap, &scope.dirty, &sections.music)) {
    DrawMusicSection(scope.dc, sections.music);
  }
  if (IntersectRect(&overlap, &scope.dirty, &sections.energy)) {
    DrawEnergySection(scope.dc, sections.energy);
  }
  if (IntersectRect(&overlap, &scope.dirty, &sections.news)) {
    DrawNewsSection(scope.dc, sections.news);
  }
}

void Renderer::PaintNativeRadar(HWND hwnd) {
  NativePanelPaintScope scope(*this, hwnd);
  if (!scope.Valid()) return;
  const RECT& bounds = scope.bounds;
  const int radius = CardRadius(bounds);
  HRGN rounded = CreateRoundRectRgn(bounds.left, bounds.top, bounds.right + 1,
                                    bounds.bottom + 1, radius, radius);
  ExtSelectClipRgn(scope.dc, rounded, RGN_AND);
  DeleteObject(rounded);

  bool hasFrame = false;
  std::wstring timeText;
  {
    std::lock_guard lock(radarFrameMutex_);
    hasFrame = radarFrameBitmap_ != nullptr;
    timeText = radarTimeText_;
    if (hasFrame) StretchRadarInto(scope.dc, bounds, radarFrameBitmap_);
  }
  HGDIOBJ previousFont = SelectObject(scope.dc, TierFont(FontTier::Small));
  if (!hasFrame) {
    HBRUSH surface = CreateSolidBrush(kWidgetSurface);
    FillRect(scope.dc, &bounds, surface);
    DeleteObject(surface);
    SetTextColor(scope.dc, kWidgetMuted);
    DrawTextInRect(scope.dc, L"レーダー画像を準備中", bounds,
                   DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  }

  const std::wstring chipText =
      L"雨雲レーダー " + (timeText.empty() ? L"--:--" : timeText);
  RECT measure{};
  DrawTextW(scope.dc, chipText.c_str(), -1, &measure,
            DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
  const int chipPadX = std::max(8, SpanX(bounds, 8));
  const int chipPadY = std::max(4, SpanY(bounds, 8));
  const int chipMargin = std::max(10, SpanY(bounds, 25));
  const RECT chip{bounds.left + chipMargin, bounds.top + chipMargin,
                  bounds.left + chipMargin + (measure.right - measure.left) + chipPadX * 2,
                  bounds.top + chipMargin + (measure.bottom - measure.top) + chipPadY * 2};
  FillRoundRectTranslucent(scope.dc, chip, RGB(0, 0, 0), chip.bottom - chip.top, 150);
  SetTextColor(scope.dc, kWidgetText);
  DrawTextInRect(scope.dc, chipText, chip, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  SelectObject(scope.dc, previousFont);

  SelectClipRgn(scope.dc, nullptr);
  DrawCardOutline(scope.dc, bounds, radius);
}

void Renderer::DrawClockSection(HDC dc, const RECT& card) {
  DrawSectionCard(dc, card);
  const RECT content = CardContentRect(card);
  SYSTEMTIME now{};
  GetLocalTime(&now);

  RECT dateRect{content.left, content.top, content.right, content.top + SpanY(content, 320)};
  HGDIOBJ previousFont = SelectObject(dc, TierFont(FontTier::Small));
  SetTextColor(dc, kWidgetMuted);
  DrawTextInRect(dc, DateText(now), dateRect, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

  RECT timeRect{content.left, dateRect.bottom, content.right, content.bottom};
  SelectObject(dc, TierFont(FontTier::Large));
  SetTextColor(dc, kWidgetText);
  DrawTextInRect(dc, TimeText(now), timeRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  SelectObject(dc, previousFont);
}

void Renderer::DrawAirSection(HDC dc, const RECT& card) {
  DrawSectionCard(dc, card);
  const RECT content = CardContentRect(card);
  HGDIOBJ previousFont = SelectObject(dc, TierFont(FontTier::Small));
  SetTextColor(dc, kWidgetSubtle);
  RECT header{content.left, content.top, content.right, content.top + CardHeaderHeight(content)};
  DrawTextInRect(dc, L"室内環境", header, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  const RECT body = CardBodyRect(content);

  const bool connected = nativeSensors_.co2Connected;
  const COLORREF co2Color = !connected ? kWidgetMuted
      : nativeSensors_.co2 >= 1500 ? kWidgetDanger
      : nativeSensors_.co2 >= 1000 ? kWidgetWarning
      : kWidgetGreen;
  struct AirStat {
    std::wstring label;
    std::wstring value;
    COLORREF color;
  };
  const std::array<AirStat, 3> stats{{
      {L"CO2", connected ? std::to_wstring(nativeSensors_.co2) + L" ppm" : L"--- ppm", co2Color},
      {L"温度", connected ? Fixed(nativeSensors_.temperatureCorrected, 1) + L"℃" : L"--.-℃",
       kWidgetText},
      {L"湿度", connected ? Fixed(nativeSensors_.humidityCorrected, 0) + L"%" : L"--%",
       kWidgetText},
  }};
  const int statsHeight = SpanY(body, 270);
  const int statsGap = std::max(4, SpanX(body, 30));
  const int cardWidth = (static_cast<int>(body.right - body.left) - statsGap * 2) / 3;
  for (int i = 0; i < 3; ++i) {
    const int left = body.left + i * (cardWidth + statsGap);
    RECT labelRect{left, body.top, left + cardWidth, body.top + statsHeight * 40 / 100};
    SetTextColor(dc, kWidgetSubtle);
    SelectObject(dc, TierFont(FontTier::Small));
    DrawTextInRect(dc, stats[i].label, labelRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    RECT valueRect{left, labelRect.bottom, left + cardWidth, body.top + statsHeight};
    SetTextColor(dc, stats[i].color);
    SelectObject(dc, TierFont(FontTier::Medium));
    DrawTextInRect(dc, stats[i].value, valueRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  }

  const int64_t now = UnixMillis();
  constexpr int64_t kWindowMs = 24LL * 60 * 60 * 1000;
  const int64_t cutoff = now - kWindowMs;
  std::vector<AirHistorySample> samples;
  samples.reserve(nativeAirHistory_.size());
  double co2Min = std::numeric_limits<double>::max();
  double co2Max = std::numeric_limits<double>::lowest();
  double temperatureMin = std::numeric_limits<double>::max();
  double temperatureMax = std::numeric_limits<double>::lowest();
  double humidityMin = std::numeric_limits<double>::max();
  double humidityMax = std::numeric_limits<double>::lowest();
  for (const auto& sample : nativeAirHistory_) {
    if (sample.timestamp < cutoff || sample.co2 < 250 || sample.co2 > 10000 ||
        sample.temperature < -40 || sample.temperature > 85 ||
        sample.humidity < 0 || sample.humidity > 100) {
      continue;
    }
    samples.push_back(sample);
    co2Min = std::min(co2Min, static_cast<double>(sample.co2));
    co2Max = std::max(co2Max, static_cast<double>(sample.co2));
    temperatureMin = std::min(temperatureMin, sample.temperature);
    temperatureMax = std::max(temperatureMax, sample.temperature);
    humidityMin = std::min(humidityMin, sample.humidity);
    humidityMax = std::max(humidityMax, sample.humidity);
  }

  const int legendTop = body.top + statsHeight + SpanY(body, 60);
  const int legendHeight = std::clamp(SpanY(body, 110), 14, 26);
  SelectObject(dc, TierFont(FontTier::Small));
  RECT legend{body.left, legendTop, body.right, legendTop + legendHeight};
  SetTextColor(dc, kWidgetSubtle);
  DrawTextInRect(dc, L"24時間の推移", legend, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  const std::array<std::pair<const wchar_t*, COLORREF>, 3> legendKeys{{
      {L"CO2", kWidgetGreen}, {L"温度", kWidgetOrange}, {L"湿度", kWidgetBlueMuted},
  }};
  const int keyWidth = std::max(34, SpanX(body, 120));
  int keyRight = body.right;
  for (int i = 2; i >= 0; --i) {
    RECT keyRect{keyRight - keyWidth, legend.top, keyRight, legend.bottom};
    SetTextColor(dc, legendKeys[static_cast<size_t>(i)].second);
    DrawTextInRect(dc, legendKeys[static_cast<size_t>(i)].first, keyRect,
                   DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    keyRight -= keyWidth;
  }

  const int axisHeight = std::clamp(SpanY(body, 100), 12, 22);
  RECT plot{body.left, legend.bottom + SpanY(body, 30), body.right,
            body.bottom - axisHeight - SpanY(body, 15)};
  HPEN gridPen = CreatePen(PS_SOLID, 1, kWidgetTrack);
  HGDIOBJ previousPen = SelectObject(dc, gridPen);
  for (int i = 0; i < 4; ++i) {
    const int y = plot.top + (plot.bottom - plot.top) * i / 3;
    MoveToEx(dc, plot.left, y, nullptr);
    LineTo(dc, plot.right, y);
  }
  SelectObject(dc, previousPen);
  DeleteObject(gridPen);

  if (!samples.empty() && plot.bottom > plot.top + 4) {
    DrawHistoryLine(dc, samples, plot, cutoff, kWindowMs, co2Min - 80, co2Max + 80, kWidgetGreen, 2,
                    [](const AirHistorySample& s) { return static_cast<double>(s.co2); });
    DrawHistoryLine(dc, samples, plot, cutoff, kWindowMs, temperatureMin - 0.5, temperatureMax + 0.5,
                    kWidgetOrange, 1,
                    [](const AirHistorySample& s) { return s.temperature; });
    DrawHistoryLine(dc, samples, plot, cutoff, kWindowMs, humidityMin - 2, humidityMax + 2,
                    kWidgetBlueMuted, 1,
                    [](const AirHistorySample& s) { return s.humidity; });
  } else {
    SetTextColor(dc, kWidgetSubtle);
    DrawTextInRect(dc, L"履歴を取得中", plot, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  }

  SetTextColor(dc, kWidgetSubtle);
  RECT axis{body.left, body.bottom - axisHeight, body.right, body.bottom};
  DrawTextInRect(dc, L"24時間前", axis, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  DrawTextInRect(dc, L"現在", axis, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
  SelectObject(dc, previousFont);
}

void Renderer::DrawWeatherSection(HDC dc, const RECT& card) {
  DrawSectionCard(dc, card);
  const RECT content = CardContentRect(card);
  const auto& hours = nativeDashboard_.weatherHours;
  const size_t count = std::min<size_t>(5, hours.size());
  double maxPop = 0;
  for (size_t i = 0; i < count; ++i) {
    if (std::isfinite(hours[i].precipitationProbability)) {
      maxPop = std::max(maxPop, hours[i].precipitationProbability);
    }
  }

  HGDIOBJ previousFont = SelectObject(dc, TierFont(FontTier::Small));
  RECT header{content.left, content.top, content.right, content.top + CardHeaderHeight(content)};
  SetTextColor(dc, kWidgetSubtle);
  DrawTextInRect(dc, L"天気予報", header, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  if (nativeDashboard_.weatherStatus.state == PanelDataState::Ok) {
    SetTextColor(dc, maxPop >= 70 ? kWidgetBlue : maxPop >= 40 ? kWidgetBlueMuted : kWidgetMuted);
    DrawTextInRect(dc,
                   L"降水確率 " + std::to_wstring(static_cast<int>(std::round(maxPop))) + L"%",
                   header, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
  } else {
    DrawHeaderStatus(dc, header, nativeDashboard_.weatherStatus);
  }

  const RECT body = CardBodyRect(content);
  const int bodyHeight = std::max(1L, body.bottom - body.top);
  const int columnGap = std::max(4, SpanX(body, 25));
  constexpr int kSlotCount = 5;
  const int columnWidth = std::max(
      1, (static_cast<int>(body.right - body.left) - columnGap * (kSlotCount - 1)) / kSlotCount);
  for (int i = 0; i < kSlotCount; ++i) {
    const int left = body.left + i * (columnWidth + columnGap);
    const RECT column{left, body.top, left + columnWidth, body.bottom};
    const bool hasData = i < static_cast<int>(count);

    SetTextColor(dc, kWidgetMuted);
    SelectObject(dc, TierFont(FontTier::Small));
    RECT hourRect{column.left, column.top, column.right, column.top + bodyHeight * 18 / 100};
    const std::wstring hour = hasData
        ? std::to_wstring(hours[static_cast<size_t>(i)].hour) + L"時" : L"--";
    DrawTextInRect(dc, hour, hourRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    if (hasData && !hours[static_cast<size_t>(i)].icon.empty()) {
      const int iconWidth = std::min(columnWidth, bodyHeight * 34 / 100 * 90 / 48);
      const int iconHeight = std::max(12, iconWidth * 48 / 90);
      const int iconTop = column.top + bodyHeight * 22 / 100;
      RECT iconRect{column.left + (columnWidth - iconWidth) / 2, iconTop,
                    column.left + (columnWidth + iconWidth) / 2, iconTop + iconHeight};
      DrawPremultipliedBitmap(dc,
          NativeWeatherIconBitmap(hours[static_cast<size_t>(i)].icon,
                                  IsWeatherNightHour(hours[static_cast<size_t>(i)].hour),
                                  iconWidth, iconHeight),
          iconRect);
    }

    SetTextColor(dc, kWidgetText);
    SelectObject(dc, TierFont(FontTier::Medium));
    RECT tempRect{column.left, column.top + bodyHeight * 56 / 100,
                  column.right, column.top + bodyHeight * 80 / 100};
    const std::wstring temp = hasData
        ? NumberOrDash(hours[static_cast<size_t>(i)].temperature, 0) + L"℃" : L"--";
    DrawTextInRect(dc, temp, tempRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    const double rainMm = hasData ? hours[static_cast<size_t>(i)].rainMm
                                  : std::numeric_limits<double>::quiet_NaN();
    SetTextColor(dc, std::isfinite(rainMm) && rainMm > 0 ? kWidgetBlueMuted : kWidgetSubtle);
    SelectObject(dc, TierFont(FontTier::Small));
    RECT rainRect{column.left, column.top + bodyHeight * 80 / 100, column.right, column.bottom};
    const std::wstring rain = hasData ? NumberOrDash(rainMm, 0) + L"mm" : L"--";
    DrawTextInRect(dc, rain, rainRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  }
  SelectObject(dc, previousFont);
}

void Renderer::DrawControlsSection(HDC dc, const RECT& card) {
  DrawSectionCard(dc, card);
  const RECT content = CardContentRect(card);
  const ControlsRects rects = ControlsFromContent(content);

  HGDIOBJ previousFont = SelectObject(dc, TierFont(FontTier::Small));
  const std::array<std::pair<std::wstring, RECT>, 2> buttons{{
      {L"更新", rects.update},
      {L"再起動", rects.restart},
  }};
  for (const auto& [label, rect] : buttons) {
    DrawWidgetCard(dc, rect, kWidgetSurfaceAlt, CardRadius(rect));
    SetTextColor(dc, kWidgetText);
    DrawTextInRect(dc, label, rect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  }

  if (!nativeToast_.empty()) {
    SetTextColor(dc, kWidgetWarning);
    DrawTextInRect(dc, nativeToast_, rects.status, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
  } else {
    std::wstring radarTime;
    bool hasFrame = false;
    {
      std::lock_guard lock(radarFrameMutex_);
      radarTime = radarTimeText_;
      hasFrame = radarFrameBitmap_ != nullptr;
    }
    const std::wstring radarStatus = hasFrame
        ? L"レーダー更新: " + (radarTime.empty() ? L"--:--" : radarTime)
        : L"レーダーを準備中";
    const int statusHeight = static_cast<int>(rects.status.bottom - rects.status.top);
    RECT firstLine = rects.status;
    firstLine.bottom = rects.status.top + statusHeight / 2;
    RECT secondLine = rects.status;
    secondLine.top = firstLine.bottom;
    SetTextColor(dc, kWidgetMuted);
    DrawTextInRect(dc, radarStatus, firstLine,
                   DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
    if (!nativeAppVersion_.empty()) {
      SetTextColor(dc, kWidgetSubtle);
      DrawTextInRect(dc, L"バージョン " + nativeAppVersion_, secondLine,
                     DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
    }
  }
  SelectObject(dc, previousFont);
}

void Renderer::DrawMusicSection(HDC dc, const RECT& card) {
  DrawSectionCard(dc, card);
  const RECT content = CardContentRect(card);
  HGDIOBJ previousFont = SelectObject(dc, TierFont(FontTier::Small));
  SetTextColor(dc, kWidgetSubtle);
  RECT header{content.left, content.top, content.right, content.top + CardHeaderHeight(content)};
  DrawTextInRect(dc, L"ミュージック", header, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  const RECT body = CardBodyRect(content);

  const int64_t nowMs = UnixMillis();
  const NativePlaybackRender playbackA = ResolveNativePlayback(0, nowMs);
  const NativePlaybackRender playbackB = ResolveNativePlayback(1, nowMs);

  const auto drawRow = [&](int row, bool muted,
                           const NativePlaybackRender& playback,
                           const std::wstring& fallbackTitle, const std::wstring& detail) {
    const MusicRowRects rects = MusicRowFromBody(body, row);
    const RECT& rowRect = rects.row;
    const int rowHeight = static_cast<int>(rowRect.bottom - rowRect.top);
    const int pad = std::max(6, rowHeight * 10 / 100);

    const int artworkRadius = std::max(6, rowHeight * 14 / 100);
    DrawWidgetCard(dc, rects.artwork, kWidgetSurfaceAlt, artworkRadius);
    if (playback.hasTrack) {
      const int saved = SaveDC(dc);
      HRGN artworkClip = CreateRoundRectRgn(rects.artwork.left, rects.artwork.top,
                                            rects.artwork.right + 1, rects.artwork.bottom + 1,
                                            artworkRadius, artworkRadius);
      ExtSelectClipRgn(dc, artworkClip, RGN_AND);
      DeleteObject(artworkClip);
      DrawPremultipliedBitmap(dc,
                              NativeArtworkBitmap(playback.track.artwork,
                                                  rects.artwork.right - rects.artwork.left,
                                                  rects.artwork.bottom - rects.artwork.top),
                              rects.artwork);
      RestoreDC(dc, saved);
    }

    const std::wstring title = playback.hasTrack ? playback.track.title : fallbackTitle;
    const std::wstring artist = playback.hasTrack ? playback.track.artist : L"";
    const bool withProgress = playback.hasTrack && playback.track.durationMs > 0;
    const int textLeft = rects.artwork.right + pad;
    const int textRight = rects.button.left - pad;

    SetTextColor(dc, kWidgetText);
    SelectObject(dc, TierFont(FontTier::Medium));
    RECT titleRect{textLeft, rowRect.top + rowHeight * 6 / 100, textRight,
                   rowRect.top + rowHeight * 42 / 100};
    DrawTextInRect(dc, title.empty() ? L"--" : title, titleRect,
                   DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

    SetTextColor(dc, kWidgetMuted);
    SelectObject(dc, TierFont(FontTier::Small));
    RECT artistRect{textLeft, titleRect.bottom, textRight,
                    withProgress ? rowRect.top + rowHeight * 62 / 100 : rowRect.bottom - pad};
    DrawTextInRect(dc, artist.empty() ? detail : artist, artistRect,
                   DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

    if (withProgress) {
      RECT barRect{textLeft, rowRect.top + rowHeight * 66 / 100,
                   textRight, rowRect.top + rowHeight * 72 / 100};
      DrawWidgetPill(dc, barRect, kWidgetTrack);
      const double ratio = std::clamp(
          static_cast<double>(playback.progressMs) / static_cast<double>(playback.track.durationMs),
          0.0, 1.0);
      RECT fillRect = barRect;
      fillRect.right = barRect.left +
          static_cast<int>((barRect.right - barRect.left) * ratio);
      if (fillRect.right > fillRect.left) {
        DrawWidgetPill(dc, fillRect, playback.playing ? kWidgetGreen : kWidgetMuted);
      }

      SetTextColor(dc, kWidgetSubtle);
      RECT timeRect{textLeft, barRect.bottom + rowHeight * 3 / 100,
                    textRight, rowRect.bottom - rowHeight * 3 / 100};
      DrawTextInRect(dc, TrackTimeText(playback.progressMs), timeRect,
                     DT_LEFT | DT_SINGLELINE | DT_VCENTER);
      DrawTextInRect(dc, TrackTimeText(playback.track.durationMs), timeRect,
                     DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }

    DrawWidgetCard(dc, rects.button, muted ? kWidgetDangerSurface : kWidgetSuccessSurface,
                   CardRadius(rects.button));
    SetTextColor(dc, muted ? kWidgetDanger : kWidgetGreen);
    SelectObject(dc, TierFont(FontTier::Small));
    DrawTextInRect(dc, muted ? L"音声OFF" : L"音声ON", rects.button,
                   DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  };

  const std::wstring detail = nativeStationhead_.loginRequired ? L"ログイン待ち"
      : nativeStationhead_.processFailed ? L"WebView再起動待ち"
      : nativeStationhead_.audioPlaying ? L"再生中"
      : nativeStationhead_.created ? L"接続中"
      : L"起動待ち";
  drawRow(0, nativeStationhead_.audioMuted, playbackA, L"", detail);
  drawRow(1, nativeStationhead_.secondaryAudioMuted, playbackB,
          L"Buddy46",
          playbackB.available && !playbackB.hasTrack ? L"次の曲を待機中"
              : nativeStationhead_.secondaryAudioMuted ? L"音声OFF" : L"音声ON");
  SelectObject(dc, previousFont);
}

void Renderer::DrawEnergySection(HDC dc, const RECT& card) {
  DrawSectionCard(dc, card);
  const RECT content = CardContentRect(card);
  HGDIOBJ previousFont = SelectObject(dc, TierFont(FontTier::Small));
  SetTextColor(dc, kWidgetSubtle);
  RECT header{content.left, content.top, content.right, content.top + CardHeaderHeight(content)};
  DrawTextInRect(dc, L"電力使用量", header, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  DrawHeaderStatus(dc, header, nativeDashboard_.octopusStatus);
  const RECT body = CardBodyRect(content);

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

  const int summaryHeight = SpanY(body, 300);
  const int summaryGap = std::max(4, SpanX(body, 40));
  const int summaryWidth = (static_cast<int>(body.right - body.left) - summaryGap) / 2;
  for (int i = 0; i < 2; ++i) {
    const int left = body.left + i * (summaryWidth + summaryGap);
    RECT labelRect{left, body.top, left + summaryWidth, body.top + summaryHeight * 30 / 100};
    SetTextColor(dc, kWidgetSubtle);
    SelectObject(dc, TierFont(FontTier::Small));
    DrawTextInRect(dc, std::get<0>(summary[static_cast<size_t>(i)]), labelRect,
                   DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    RECT valueRect{left, labelRect.bottom, left + summaryWidth,
                   body.top + summaryHeight * 75 / 100};
    SetTextColor(dc, kWidgetText);
    SelectObject(dc, TierFont(FontTier::Medium));
    DrawTextInRect(dc, std::get<1>(summary[static_cast<size_t>(i)]), valueRect,
                   DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    RECT costRect{left, valueRect.bottom, left + summaryWidth, body.top + summaryHeight};
    SetTextColor(dc, kWidgetSubtle);
    SelectObject(dc, TierFont(FontTier::Small));
    DrawTextInRect(dc, std::get<2>(summary[static_cast<size_t>(i)]), costRect,
                   DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  }

  const int plugHeight = std::clamp(SpanY(body, 110), 12, 22);
  RECT chart{body.left, body.top + summaryHeight + SpanY(body, 40),
             body.right, body.bottom - plugHeight - SpanY(body, 25)};
  if (chart.bottom > chart.top + 30 && !nativeDashboard_.octopusProfile.empty()) {
    double maximum = 0.1;
    for (const auto& item : nativeDashboard_.octopusProfile) {
      if (item.currentDays == 7 && std::isfinite(item.currentAverage)) {
        maximum = std::max(maximum, item.currentAverage);
      }
      if (item.previousDays == 7 && std::isfinite(item.previousAverage)) {
        maximum = std::max(maximum, item.previousAverage);
      }
    }
    maximum *= 1.1;

    const int chartHeight = static_cast<int>(chart.bottom - chart.top);
    const int legendHeight = std::clamp(chartHeight * 20 / 100, 12, 22);
    const int axisHeight = std::clamp(chartHeight * 15 / 100, 10, 18);
    const int yLabelWidth = std::max(24, SpanX(body, 70));
    RECT legend{chart.left, chart.top, chart.right, chart.top + legendHeight};
    SelectObject(dc, TierFont(FontTier::Small));

    const int legendHalf = (static_cast<int>(legend.right - legend.left)) / 2;
    const int swatchWidth = std::max(14, SpanX(body, 45));
    const int swatchHeight = std::max(3, legendHeight * 20 / 100);
    RECT currentSwatch{legend.left, legend.top + (legendHeight - swatchHeight) / 2,
                       legend.left + swatchWidth,
                       legend.top + (legendHeight - swatchHeight) / 2 + swatchHeight};
    RECT previousSwatch{legend.left + legendHalf, currentSwatch.top,
                        legend.left + legendHalf + swatchWidth, currentSwatch.bottom};
    DrawWidgetCard(dc, currentSwatch, kWidgetCyan, 2, 220);
    DrawWidgetCard(dc, previousSwatch, kWidgetPurple, 2, 120);

    const bool currentComplete = std::all_of(
        nativeDashboard_.octopusProfile.begin(), nativeDashboard_.octopusProfile.end(),
        [](const OctopusProfileData& point) { return point.currentDays == 7; });
    const bool previousComplete = std::all_of(
        nativeDashboard_.octopusProfile.begin(), nativeDashboard_.octopusProfile.end(),
        [](const OctopusProfileData& point) { return point.previousDays == 7; });
    std::wstring currentLegend = nativeDashboard_.currentEnergyLabel;
    if (!nativeDashboard_.currentEnergyDateRange.empty()) {
      currentLegend += L" " + nativeDashboard_.currentEnergyDateRange;
    }
    if (!currentComplete) currentLegend += L"（データ不足）";
    std::wstring previousLegend = nativeDashboard_.previousEnergyLabel;
    if (!nativeDashboard_.previousEnergyDateRange.empty()) {
      previousLegend += L" " + nativeDashboard_.previousEnergyDateRange;
    }
    if (!previousComplete) previousLegend += L"（データ不足）";
    SetTextColor(dc, kWidgetMuted);
    RECT currentLegendRect{currentSwatch.right + SpanX(body, 15), legend.top,
                           legend.left + legendHalf - SpanX(body, 10), legend.bottom};
    RECT previousLegendRect{previousSwatch.right + SpanX(body, 15), legend.top,
                            legend.right, legend.bottom};
    DrawTextInRect(dc, currentLegend, currentLegendRect,
                   DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    DrawTextInRect(dc, previousLegend, previousLegendRect,
                   DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    RECT plot{chart.left + yLabelWidth, legend.bottom + SpanY(body, 15),
              chart.right - SpanX(body, 10), chart.bottom - axisHeight};
    if (plot.right > plot.left + 20 && plot.bottom > plot.top + 10) {
      for (int line = 0; line <= 4; ++line) {
        const int y = plot.bottom - static_cast<int>((plot.bottom - plot.top) * line / 4.0);
        RECT grid{plot.left, y, plot.right, y + 1};
        AlphaBlendSolidColor(dc, grid, RGB(255, 255, 255), line == 0 ? 70 : 35);
      }

      SetTextColor(dc, kWidgetSubtle);
      RECT maxLabel{chart.left, plot.top - SpanY(body, 20),
                    plot.left - SpanX(body, 10), plot.top + SpanY(body, 50)};
      DrawTextInRect(dc, Fixed(maximum, maximum >= 1.0 ? 1 : 2), maxLabel,
                     DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
      RECT zeroLabel{chart.left, plot.bottom - SpanY(body, 35),
                     plot.left - SpanX(body, 10), plot.bottom + SpanY(body, 35)};
      DrawTextInRect(dc, L"0", zeroLabel, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

      const int count = static_cast<int>(nativeDashboard_.octopusProfile.size());
      constexpr int profileSlots = 48;
      const auto xFor = [&](int slot) {
        return static_cast<int>(plot.left + (plot.right - plot.left) * slot /
            static_cast<double>(profileSlots));
      };
      const auto yFor = [&](double value) {
        const double ratio = std::clamp(value / maximum, 0.0, 1.0);
        return static_cast<int>(plot.bottom - (plot.bottom - plot.top) * ratio);
      };
      const auto blendOnBackground = [](COLORREF foreground, BYTE alpha) {
        const int inverse = 255 - alpha;
        const int red = (GetRValue(foreground) * alpha + GetRValue(kWidgetSurface) * inverse) / 255;
        const int green = (GetGValue(foreground) * alpha + GetGValue(kWidgetSurface) * inverse) / 255;
        const int blue = (GetBValue(foreground) * alpha + GetBValue(kWidgetSurface) * inverse) / 255;
        return RGB(red, green, blue);
      };
      const auto drawSeries = [&](bool current, COLORREF color, int width) {
        HPEN pen = CreatePen(PS_SOLID, width, color);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        bool started = false;
        for (int index = 0; index < count; ++index) {
          const auto& point = nativeDashboard_.octopusProfile[static_cast<size_t>(index)];
          const bool complete = current ? point.currentDays == 7 : point.previousDays == 7;
          const double value = complete
              ? (current ? point.currentAverage : point.previousAverage)
              : std::numeric_limits<double>::quiet_NaN();
          if (!std::isfinite(value)) {
            started = false;
            continue;
          }
          const int x = xFor(index);
          const int y = yFor(value);
          if (!started) MoveToEx(dc, x, y, nullptr);
          else LineTo(dc, x, y);
          started = true;
        }
        SelectObject(dc, oldPen);
        DeleteObject(pen);
      };

      drawSeries(false, blendOnBackground(kWidgetPurple, 120), 3);
      drawSeries(true, kWidgetCyan, 3);

      const std::array<std::pair<int, const wchar_t*>, 5> ticks{{
          {0, L"0:00"}, {12, L"6:00"}, {24, L"12:00"}, {36, L"18:00"}, {48, L"24:00"},
      }};
      SetTextColor(dc, kWidgetSubtle);
      for (const auto& [slot, label] : ticks) {
        const int x = xFor(slot);
        RECT tick{x - SpanX(body, 55), plot.bottom,
                  x + SpanX(body, 55), chart.bottom};
        DrawTextInRect(dc, label, tick, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
      }
    }
  } else if (chart.bottom > chart.top + 30) {
    SelectObject(dc, TierFont(FontTier::Small));
    SetTextColor(dc, kWidgetSubtle);
    DrawTextInRect(dc, L"使用量プロファイルを取得中", chart,
                   DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  }

  SelectObject(dc, TierFont(FontTier::Small));
  SetTextColor(dc, kWidgetSubtle);
  RECT plugRect{body.left, body.bottom - plugHeight, body.right, body.bottom};
  std::wstring plugs = L"Plug Mini情報なし";
  if (!nativeDashboard_.switchBotDevices.empty()) {
    plugs.clear();
    const size_t deviceCount = std::min<size_t>(2, nativeDashboard_.switchBotDevices.size());
    for (size_t i = 0; i < deviceCount; ++i) {
      if (i) plugs += L"  ";
      plugs += nativeDashboard_.switchBotDevices[i].name + L": " +
               nativeDashboard_.switchBotDevices[i].state;
    }
  }
  DrawTextInRect(dc, plugs, plugRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
  SelectObject(dc, previousFont);
}

void Renderer::DrawNewsSection(HDC dc, const RECT& card) {
  DrawSectionCard(dc, card);
  const RECT content = CardContentRect(card);
  const NewsItemData* item = nullptr;
  if (!nativeDashboard_.newsItems.empty()) {
    const size_t index = static_cast<size_t>(std::max(0, nativeNewsIndex_)) %
                         nativeDashboard_.newsItems.size();
    item = &nativeDashboard_.newsItems[index];
  }
  const std::wstring title = item ? item->title : L"ニュース取得待ち";
  const std::wstring description = item ? item->description : L"";

  const int contentHeight = std::max(1L, content.bottom - content.top);
  RECT titleRow{content.left, content.top, content.right,
                content.top + contentHeight * 55 / 100};
  RECT descRow{content.left, titleRow.bottom, content.right, content.bottom};

  HGDIOBJ previousFont = SelectObject(dc, TierFont(FontTier::Small));
  const std::wstring chipLabel = L"ニュース";
  RECT measure{};
  DrawTextW(dc, chipLabel.c_str(), -1, &measure, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
  const int chipPadX = std::max(8, SpanX(content, 10));
  const int chipHeight = std::min(static_cast<int>(titleRow.bottom - titleRow.top),
                                  static_cast<int>(measure.bottom - measure.top) + 8);
  const int chipTop = titleRow.top +
      (static_cast<int>(titleRow.bottom - titleRow.top) - chipHeight) / 2;
  const RECT chip{titleRow.left, chipTop,
                  titleRow.left + (measure.right - measure.left) + chipPadX * 2,
                  chipTop + chipHeight};
  FillRoundRectTranslucent(dc, chip, kWidgetBlue, chipHeight, 70);
  SetTextColor(dc, kWidgetBlueMuted);
  DrawTextInRect(dc, chipLabel, chip, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

  RECT titleRect{chip.right + std::max(8, SpanX(content, 12)), titleRow.top,
                 titleRow.right, titleRow.bottom};
  SetTextColor(dc, kWidgetText);
  SelectObject(dc, TierFont(FontTier::Medium));
  DrawTextInRect(dc, title, titleRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

  if (!description.empty()) {
    SetTextColor(dc, kWidgetMuted);
    SelectObject(dc, TierFont(FontTier::Small));
    DrawTextInRect(dc, description, descRow,
                   DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
  }
  SelectObject(dc, previousFont);
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

  return CacheNativeBitmap(key, DecodeImageFileToBitmap(dataDir_ / relative, width, height));
}

HBITMAP Renderer::NativeWeatherIconBitmap(const std::wstring& icon, bool night, int width, int height) {
  if (icon.empty() || width <= 0 || height <= 0) return nullptr;
  for (wchar_t ch : icon) {
    if (ch < L'0' || ch > L'9') return nullptr;
  }
  const std::wstring fileName = icon + (night ? L"_night.png" : L"_day.png");
  const std::wstring key = L"weather-icon:" + fileName + L"#" +
                           std::to_wstring(width) + L"x" + std::to_wstring(height);
  auto found = nativeArtworkBitmaps_.find(key);
  if (found != nativeArtworkBitmaps_.end()) {
    found->second.lastUsed = ++nativeArtworkUseCounter_;
    return found->second.bitmap;
  }

  const auto decodeIcon = [&](const std::wstring& name) {
    return DecodeImageFileToBitmap(rootDir_ / L"ui" / L"weather-icons" / name, width, height);
  };
  HBITMAP bitmap = decodeIcon(fileName);
  if (!bitmap && night) bitmap = decodeIcon(icon + L"_day.png");
  if (!bitmap) {
    const wchar_t family = icon.front();
    const std::wstring fallback = family == L'2' ? L"200" : family == L'3' ? L"300" :
                                  family == L'4' ? L"400" : L"100";
    bitmap = decodeIcon(fallback + (night ? L"_night.png" : L"_day.png"));
    if (!bitmap && night) bitmap = decodeIcon(fallback + L"_day.png");
  }

  return CacheNativeBitmap(key, bitmap);
}

HBITMAP Renderer::CacheNativeBitmap(const std::wstring& key, HBITMAP bitmap) {
  if (nativeArtworkBitmaps_.size() >= kNativeBitmapCacheLimit) {
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
}
