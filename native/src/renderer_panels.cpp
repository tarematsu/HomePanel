#include "web_renderer.h"
#include "wic_image.h"

namespace hp {
namespace {
constexpr int kNativeTopId = 101;
constexpr int kNativeBottomId = 102;
constexpr int kNativeRadarId = 108;
constexpr size_t kNativeBitmapCacheLimit = 24;

constexpr COLORREF kWidgetBackground = kNativeDashboardBackground;
constexpr COLORREF kWidgetSurfaceAlt = RGB(24, 31, 41);
constexpr COLORREF kWidgetText = RGB(255, 255, 255);
constexpr COLORREF kWidgetMuted = RGB(255, 255, 255);
constexpr COLORREF kWidgetSubtle = RGB(255, 255, 255);

constexpr COLORREF kWidgetBlue = RGB(10, 132, 255);
constexpr COLORREF kWidgetBlueMuted = RGB(64, 156, 255);
constexpr COLORREF kWidgetGreen = RGB(48, 209, 88);
constexpr COLORREF kWidgetOrange = RGB(255, 159, 10);
constexpr COLORREF kWidgetCyan = RGB(50, 173, 230);
constexpr COLORREF kWidgetPurple = RGB(175, 82, 222);
constexpr COLORREF kWidgetTrack = RGB(34, 44, 56);
constexpr COLORREF kWidgetWarning = RGB(255, 214, 10);
constexpr COLORREF kWidgetDanger = RGB(255, 69, 58);
constexpr COLORREF kWidgetDangerSurface = RGB(42, 33, 35);
constexpr COLORREF kWidgetSuccessSurface = RGB(24, 46, 34);

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

void DrawWidgetCard(HDC dc, const RECT& rect, COLORREF color = kWidgetSurfaceAlt,
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



void DrawShadowedText(HDC dc, const std::wstring& text, RECT rect, int format, COLORREF color) {
  RECT shadowRect = rect;
  OffsetRect(&shadowRect, 0, 2);
  SetTextColor(dc, RGB(0, 0, 0));
  DrawTextInRect(dc, text, shadowRect, format);
  SetTextColor(dc, color);
  DrawTextInRect(dc, text, rect, format);
}






RECT RadarSampleRectFor(const RECT& absoluteRect, const RECT& clientBounds) {
  const int clientWidth = std::max(1L, clientBounds.right - clientBounds.left);
  const int clientHeight = std::max(1L, clientBounds.bottom - clientBounds.top);
  const double scale = std::max(static_cast<double>(clientWidth) / kRadarCanvasWidth,
                                static_cast<double>(clientHeight) / kRadarCanvasHeight);
  const double offsetX = (clientWidth - kRadarCanvasWidth * scale) / 2.0;
  const double offsetY = (clientHeight - kRadarCanvasHeight * scale) / 2.0;
  const auto toRadarX = [&](long clientX) { return (clientX - clientBounds.left - offsetX) / scale; };
  const auto toRadarY = [&](long clientY) { return (clientY - clientBounds.top - offsetY) / scale; };

  RECT sample{
      static_cast<LONG>(std::lround(toRadarX(absoluteRect.left))),
      static_cast<LONG>(std::lround(toRadarY(absoluteRect.top))),
      static_cast<LONG>(std::lround(toRadarX(absoluteRect.right))),
      static_cast<LONG>(std::lround(toRadarY(absoluteRect.bottom))),
  };
  sample.left = std::clamp<LONG>(sample.left, 0, kRadarCanvasWidth - 1);
  sample.top = std::clamp<LONG>(sample.top, 0, kRadarCanvasHeight - 1);
  sample.right = std::clamp<LONG>(sample.right, sample.left + 1, kRadarCanvasWidth);
  sample.bottom = std::clamp<LONG>(sample.bottom, sample.top + 1, kRadarCanvasHeight);
  return sample;
}

void StretchRadarSampleInto(HDC destDc, const RECT& destRect, HBITMAP radarBitmap, const RECT& srcRect) {
  if (!radarBitmap) {
    FillWidgetBackground(destDc, destRect);
    return;
  }
  HDC sourceDc = CreateCompatibleDC(destDc);
  if (!sourceDc) {
    FillWidgetBackground(destDc, destRect);
    return;
  }
  HGDIOBJ previous = SelectObject(sourceDc, radarBitmap);
  SetStretchBltMode(destDc, HALFTONE);
  SetBrushOrgEx(destDc, 0, 0, nullptr);
  StretchBlt(destDc, destRect.left, destRect.top, destRect.right - destRect.left, destRect.bottom - destRect.top,
            sourceDc, srcRect.left, srcRect.top, srcRect.right - srcRect.left, srcRect.bottom - srcRect.top,
            SRCCOPY);
  SelectObject(sourceDc, previous);
  DeleteDC(sourceDc);
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




void DrawHorizontalDivider(HDC dc, const RECT& area, int y) {
  const int inset = static_cast<int>((area.right - area.left) * 225 / 1000);
  const RECT line{area.left + inset, y, area.right - inset, y + 2};
  AlphaBlendSolidColor(dc, line, RGB(0, 0, 0), 90);
}

void DrawVerticalDivider(HDC dc, const RECT& area, int x) {
  const int inset = static_cast<int>((area.bottom - area.top) * 225 / 1000);
  const RECT line{x, area.top + inset, x + 2, area.bottom - inset};
  AlphaBlendSolidColor(dc, line, RGB(0, 0, 0), 90);
}

struct ControlsButtonRects {
  RECT update{};
  RECT restart{};
  RECT toast{};
  RECT status{};
};


ControlsButtonRects ControlsButtonsFromSection(const RECT& section) {
  const int width = std::max(1L, section.right - section.left);
  const int height = std::max(1L, section.bottom - section.top);
  const int buttonWidth = SpanX(section, 380);
  const int buttonHeight = std::clamp(SpanY(section, 140), 30, 56);
  const int gap = SpanX(section, 40);
  const int totalWidth = buttonWidth * 2 + gap;
  const int left = section.left + (width - totalWidth) / 2;
  const int statusHeight = SpanY(section, 150);
  const int blockHeight = SpanY(section, 160) + SpanY(section, 400) + SpanY(section, 60) +
      buttonHeight + SpanY(section, 35) + statusHeight;
  const int blockTop = section.top + std::max(0, (height - blockHeight) / 2);
  const int bottom = blockTop + SpanY(section, 160) + SpanY(section, 400) +
      SpanY(section, 60) + buttonHeight;
  const int top = bottom - buttonHeight;
  ControlsButtonRects rects;
  rects.update = RECT{left, top, left + buttonWidth, bottom};
  rects.restart = RECT{left + buttonWidth + gap, top, left + totalWidth, bottom};
  rects.status = RECT{section.left, bottom + SpanY(section, 35),
                      section.right, bottom + SpanY(section, 35) + statusHeight};
  rects.toast = rects.status;
  return rects;
}

struct StationheadRowRects {
  RECT row{};
  RECT button{};
};



StationheadRowRects StationheadRowFromSection(const RECT& section, int row) {
  const int gap = SpanY(section, 60);
  const int rowHeight = (section.bottom - section.top - gap) / 2;
  const int top = section.top + row * (rowHeight + gap);
  StationheadRowRects rects;
  rects.row = RECT{section.left, top, section.right, top + rowHeight};
  const int buttonWidth = SpanX(section, 200);
  const int buttonHeight = std::clamp(rowHeight * 36 / 100, 26, 48);
  const int buttonRight = rects.row.right - SpanX(section, 25);
  const int buttonTop = top + (rowHeight - buttonHeight) / 2;
  rects.button = RECT{buttonRight - buttonWidth, buttonTop, buttonRight, buttonTop + buttonHeight};
  return rects;
}
}

const std::array<Renderer::NativePanelSlot, 3>& Renderer::NativePanelSlots() {
  static const std::array<NativePanelSlot, 3> slots{{
      {&Renderer::nativeRadarWindow_, &NativeDashboardLayout::radar,
       L"HomePanelNativeRadar", kNativeRadarId},
      {&Renderer::nativeTopWindow_, &NativeDashboardLayout::top,
       L"HomePanelNativeTop", kNativeTopId},
      {&Renderer::nativeBottomWindow_, &NativeDashboardLayout::bottom,
       L"HomePanelNativeBottom", kNativeBottomId},
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
      return CachedUiFont(std::clamp(clientHeight * 48 / 1000, 32, 96), FW_SEMIBOLD);
  }
}

Renderer::NativePanelPaintScope::NativePanelPaintScope(Renderer& renderer, HWND hwnd,
    const RECT& absoluteRect, BYTE tintAlpha, int cornerRadius, COLORREF tintColor)
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
  {
    std::lock_guard lock(renderer.radarFrameMutex_);
    StretchRadarSampleInto(dc, bounds, renderer.radarFrameBitmap_,
                           RadarSampleRectFor(absoluteRect, renderer.bounds_));
  }
  if (tintAlpha > 0) {
    HRGN clip = CreateRoundRectRgn(0, 0, bounds.right + 1, bounds.bottom + 1, cornerRadius * 2, cornerRadius * 2);
    SelectClipRgn(dc, clip);
    AlphaBlendSolidColor(dc, dirty, tintColor, tintAlpha);
    SelectClipRgn(dc, dirtyClip);
    DeleteObject(clip);
  }
  DeleteObject(dirtyClip);
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
  const NativePanelSections sections = SplitPanelSections(client);
  const RECT& rect = section == PanelSection::Left ? sections.left
      : section == PanelSection::Center ? sections.center : sections.right;
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
  if (sensorsChanged || historyChanged) InvalidatePanelSection(nativeTopWindow_, PanelSection::Left);
  if (dashboardChanged || newsChanged) InvalidatePanelSection(nativeTopWindow_, PanelSection::Center);
  if (dashboardChanged) {
    InvalidatePanelSection(nativeTopWindow_, PanelSection::Right);
    InvalidatePanelSection(nativeBottomWindow_, PanelSection::Right);
  }
  if (controlsChanged) InvalidatePanelSection(nativeBottomWindow_, PanelSection::Center);
  if (stationheadChanged) InvalidatePanelSection(nativeBottomWindow_, PanelSection::Left);
}

void Renderer::TickNativePanels(int64_t nowMs) {
  if (!nativeDashboardVisible_) return;
  if (nativeBottomWindow_ && IsWindow(nativeBottomWindow_) && IsWindowVisible(nativeBottomWindow_)) {
    InvalidatePanelSection(nativeBottomWindow_, PanelSection::Center);
    if (NativePlaybackActive(nowMs)) {
      InvalidatePanelSection(nativeBottomWindow_, PanelSection::Left);
    }
  }
}

LRESULT Renderer::HandleNativeStaticMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_ERASEBKGND:
      return 1;
    case WM_LBUTTONUP:
      if (GetDlgCtrlID(hwnd) == kNativeBottomId) {
        RECT client{};
        GetClientRect(hwnd, &client);
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const NativePanelSections sections = SplitPanelSections(client);
        const ControlsButtonRects buttons =
            ControlsButtonsFromSection(RelativeInsetRect(sections.center, 30, 60));
        if (PtInRect(&buttons.update, point)) QueueAction(UiAction::AppUpdate);
        else if (PtInRect(&buttons.restart, point)) QueueAction(UiAction::Restart);
        else {
          const RECT stationhead = RelativeInsetRect(sections.left, 30, 60);
          for (int row = 0; row < 2; ++row) {
            const StationheadRowRects rowRects = StationheadRowFromSection(stationhead, row);
            if (PtInRect(&rowRects.button, point)) {
              QueueAction(row == 0 ? UiAction::StationheadAudioToggleA
                                   : UiAction::StationheadAudioToggleB);
              break;
            }
          }
        }
        return 0;
      }
      break;
    case WM_PAINT:
      if (GetDlgCtrlID(hwnd) == kNativeTopId) PaintNativeTop(hwnd);
      else if (GetDlgCtrlID(hwnd) == kNativeBottomId) PaintNativeBottom(hwnd);
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

void Renderer::PaintNativeTop(HWND hwnd) {
  const RECT absoluteRect = ComputeNativeDashboardLayout(bounds_).top;
  NativePanelPaintScope scope(*this, hwnd, absoluteRect);
  if (!scope.Valid()) return;
  const NativePanelSections sections = SplitPanelSections(scope.bounds);
  RECT overlap{};
  if (IntersectRect(&overlap, &scope.dirty, &sections.left)) {
    DrawAirSection(scope.dc, RelativeInsetRect(sections.left, 30, 60));
  }
  if (IntersectRect(&overlap, &scope.dirty, &sections.center)) {
    DrawNewsSection(scope.dc, RelativeInsetRect(sections.center, 30, 60));
  }
  if (IntersectRect(&overlap, &scope.dirty, &sections.right)) {
    DrawEnergySection(scope.dc, RelativeInsetRect(sections.right, 30, 60));
  }
  DrawVerticalDivider(scope.dc, scope.bounds, (sections.left.right + sections.center.left) / 2);
  DrawVerticalDivider(scope.dc, scope.bounds, (sections.center.right + sections.right.left) / 2);
}

void Renderer::PaintNativeBottom(HWND hwnd) {
  const RECT absoluteRect = ComputeNativeDashboardLayout(bounds_).bottom;
  NativePanelPaintScope scope(*this, hwnd, absoluteRect);
  if (!scope.Valid()) return;
  const NativePanelSections sections = SplitPanelSections(scope.bounds);
  RECT overlap{};
  if (IntersectRect(&overlap, &scope.dirty, &sections.left)) {
    DrawStationheadSection(scope.dc, RelativeInsetRect(sections.left, 30, 60));
  }
  if (IntersectRect(&overlap, &scope.dirty, &sections.center)) {
    DrawClockControlsSection(scope.dc, RelativeInsetRect(sections.center, 30, 60));
  }
  if (IntersectRect(&overlap, &scope.dirty, &sections.right)) {
    DrawWeatherSection(scope.dc, RelativeInsetRect(sections.right, 30, 60));
  }
  DrawVerticalDivider(scope.dc, scope.bounds, (sections.left.right + sections.center.left) / 2);
  DrawVerticalDivider(scope.dc, scope.bounds, (sections.center.right + sections.right.left) / 2);
}

void Renderer::PaintNativeRadar(HWND hwnd) {
  const NativeDashboardLayout layout = ComputeNativeDashboardLayout(bounds_);
  PAINTSTRUCT paint{};
  HDC paintDc = BeginPaint(hwnd, &paint);
  if (!paintDc) return;
  RECT client{};
  GetClientRect(hwnd, &client);
  RECT dirty = paint.rcPaint;
  if (IsRectEmpty(&dirty)) dirty = client;
  IntersectRect(&dirty, &dirty, &client);
  HRGN dirtyClip = CreateRectRgnIndirect(&dirty);
  SelectClipRgn(paintDc, dirtyClip);
  {
    std::lock_guard lock(radarFrameMutex_);
    StretchRadarSampleInto(paintDc, client, radarFrameBitmap_,
                           RadarSampleRectFor(layout.radar, bounds_));
  }
  SelectClipRgn(paintDc, nullptr);
  DeleteObject(dirtyClip);
  EndPaint(hwnd, &paint);
}

void Renderer::DrawAirSection(HDC dc, const RECT& content) {
  const int64_t now = UnixMillis();
  const int statsTop = content.top;
  const int statsHeight = SpanY(content, 300);
  const int gap = SpanX(content, 40);
  const std::array<std::pair<std::wstring, std::wstring>, 3> values{{
      {L"CO2", nativeSensors_.co2Connected ? std::to_wstring(nativeSensors_.co2) + L" ppm" : L"--- ppm"},
      {L"温度", nativeSensors_.co2Connected ? Fixed(nativeSensors_.temperatureCorrected, 1) + L"℃" : L"--.-℃"},
      {L"湿度", nativeSensors_.co2Connected ? Fixed(nativeSensors_.humidityCorrected, 0) + L"%" : L"--%"},
  }};
  const int cardWidth = (content.right - content.left - gap * 2) / 3;
  for (int i = 0; i < 3; ++i) {
    RECT rect{content.left + i * (cardWidth + gap), statsTop,
              content.left + i * (cardWidth + gap) + cardWidth, statsTop + statsHeight};
    RECT labelRect{rect.left, rect.top, rect.right, rect.top + statsHeight * 40 / 100};
    SetTextColor(dc, kWidgetMuted);
    HGDIOBJ previousFont = SelectObject(dc, TierFont(FontTier::Small));
    DrawTextInRect(dc, values[i].first, labelRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    RECT valueRect{rect.left, labelRect.bottom, rect.right, rect.bottom};
    SetTextColor(dc, kWidgetText);
    SelectObject(dc, TierFont(FontTier::Medium));
    DrawTextInRect(dc, values[i].second, valueRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, previousFont);
  }

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

  const int historyTop = statsTop + statsHeight + SpanY(content, 70);
  DrawHorizontalDivider(dc, content, statsTop + statsHeight + SpanY(content, 35));
  const int legendHeight = SpanY(content, 120);
  HGDIOBJ previousFont = SelectObject(dc, TierFont(FontTier::Small));
  SetTextColor(dc, kWidgetMuted);
  RECT legend{content.left, historyTop, content.right, historyTop + legendHeight};
  DrawTextInRect(dc, L"履歴(24時間)  CO2   温度   湿度", legend, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

  const int axisHeight = SpanY(content, 120);
  RECT plot{content.left + SpanX(content, 50), legend.bottom + SpanY(content, 20),
            content.right, content.bottom - axisHeight};
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
    DrawHistoryLine(dc, samples, plot, cutoff, kWindowMs, humidityMin - 2, humidityMax + 2, kWidgetBlue, 1,
                    [](const AirHistorySample& s) { return s.humidity; });
  } else {
    DrawTextInRect(dc, L"履歴を取得中", plot, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  }

  SetTextColor(dc, kWidgetSubtle);
  RECT axis{content.left, content.bottom - axisHeight, content.right, content.bottom};
  DrawTextInRect(dc, L"24時間前", axis, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  DrawTextInRect(dc, L"現在", axis, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
  SelectObject(dc, previousFont);
}

void Renderer::DrawNewsSection(HDC dc, const RECT& content) {
  const NewsItemData* item = nullptr;
  if (!nativeDashboard_.newsItems.empty()) {
    const size_t index = static_cast<size_t>(std::max(0, nativeNewsIndex_)) % nativeDashboard_.newsItems.size();
    item = &nativeDashboard_.newsItems[index];
  }
  const std::wstring title = item ? item->title : L"ニュース取得待ち";
  const std::wstring description = item ? item->description : L"";

  RECT titleRect{content.left, content.top, content.right, content.top + SpanY(content, 220)};
  HGDIOBJ previousFont = SelectObject(dc, TierFont(FontTier::Medium));
  SetTextColor(dc, kWidgetText);
  DrawTextInRect(dc, title, titleRect, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

  if (!description.empty()) {
    RECT descRect{content.left, titleRect.bottom + SpanY(content, 40), content.right, content.bottom};
    SelectObject(dc, TierFont(FontTier::Small));
    SetTextColor(dc, kWidgetMuted);
    DrawTextInRect(dc, description, descRect, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
  }
  SelectObject(dc, previousFont);
}

void Renderer::DrawEnergySection(HDC dc, const RECT& content) {
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

  const int summaryHeight = SpanY(content, 330);
  const int summaryGap = SpanX(content, 50);
  const int summaryWidth = (content.right - content.left - summaryGap) / 2;
  HGDIOBJ previousFont = nullptr;
  for (int i = 0; i < 2; ++i) {
    RECT rect{content.left + i * (summaryWidth + summaryGap), content.top,
              content.left + i * (summaryWidth + summaryGap) + summaryWidth,
              content.top + summaryHeight};
    SetTextColor(dc, kWidgetMuted);
    previousFont = SelectObject(dc, TierFont(FontTier::Small));
    RECT labelRect{rect.left, rect.top, rect.right, rect.top + summaryHeight * 30 / 100};
    DrawTextInRect(dc, std::get<0>(summary[i]), labelRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SetTextColor(dc, kWidgetText);
    SelectObject(dc, TierFont(FontTier::Medium));
    RECT valueRect{rect.left, labelRect.bottom, rect.right, rect.top + summaryHeight * 72 / 100};
    DrawTextInRect(dc, std::get<1>(summary[i]), valueRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SetTextColor(dc, kWidgetMuted);
    SelectObject(dc, TierFont(FontTier::Small));
    RECT costRect{rect.left, valueRect.bottom, rect.right, rect.bottom};
    DrawTextInRect(dc, std::get<2>(summary[i]), costRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, previousFont);
  }

  DrawHorizontalDivider(dc, content, content.top + summaryHeight + SpanY(content, 25));
  const int plugHeight = SpanY(content, 120);
  RECT chart{content.left, content.top + summaryHeight + SpanY(content, 50),
             content.right, content.bottom - plugHeight - SpanY(content, 30)};
  if (chart.bottom > chart.top + 20 && !nativeDashboard_.octopusHistory.empty()) {
    double maximum = 1.0;
    for (const auto& item : nativeDashboard_.octopusHistory) {
      if (std::isfinite(item.value)) maximum = std::max(maximum, item.value);
      if (std::isfinite(item.previousWeekValue)) maximum = std::max(maximum, item.previousWeekValue);
    }

    const int legendHeight = SpanY(content, 100);
    const int weekdayHeight = SpanY(content, 80);
    const int valueBand = SpanY(content, 80);
    RECT legend{chart.left, chart.top, chart.right, chart.top + legendHeight};
    previousFont = SelectObject(dc, TierFont(FontTier::Small));

    std::wstring currentLegend = L"今週";
    if (nativeDashboard_.currentEnergyIsoYear > 0 && nativeDashboard_.currentEnergyIsoWeek > 0) {
      currentLegend += L" " + std::to_wstring(nativeDashboard_.currentEnergyIsoYear)
          + L" W" + std::to_wstring(nativeDashboard_.currentEnergyIsoWeek);
    }
    std::wstring previousLegend = L"先週";
    if (nativeDashboard_.previousEnergyIsoYear > 0 && nativeDashboard_.previousEnergyIsoWeek > 0) {
      previousLegend += L" " + std::to_wstring(nativeDashboard_.previousEnergyIsoYear)
          + L" W" + std::to_wstring(nativeDashboard_.previousEnergyIsoWeek);
    }

    const int legendHalf = (legend.right - legend.left) / 2;
    const int swatchSize = std::max(6, SpanY(content, 35));
    RECT currentSwatch{legend.left, legend.top + (legendHeight - swatchSize) / 2,
                       legend.left + swatchSize, legend.top + (legendHeight + swatchSize) / 2};
    RECT previousSwatch{legend.left + legendHalf, currentSwatch.top,
                        legend.left + legendHalf + swatchSize, currentSwatch.bottom};
    DrawWidgetCard(dc, currentSwatch, kWidgetCyan, 3, 210);
    DrawWidgetCard(dc, previousSwatch, kWidgetPurple, 3, 120);
    SetTextColor(dc, kWidgetMuted);
    RECT currentLegendRect{currentSwatch.right + SpanX(content, 15), legend.top,
                           legend.left + legendHalf - SpanX(content, 10), legend.bottom};
    RECT previousLegendRect{previousSwatch.right + SpanX(content, 15), legend.top,
                            legend.right, legend.bottom};
    DrawTextInRect(dc, currentLegend, currentLegendRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    DrawTextInRect(dc, previousLegend, previousLegendRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    RECT plot{chart.left, legend.bottom, chart.right, chart.bottom - weekdayHeight};
    const int count = static_cast<int>(nativeDashboard_.octopusHistory.size());
    const int step = std::max(1, static_cast<int>(plot.right - plot.left) / std::max(1, count));
    const int pairGap = std::max(1, step / 18);
    const int barWidth = std::max(2, std::min(step * 3 / 10, (step - pairGap * 3) / 2));
    const int usableHeight = std::max(1, static_cast<int>(plot.bottom - plot.top) - valueBand);

    for (int i = 0; i < count; ++i) {
      const auto& item = nativeDashboard_.octopusHistory[i];
      const int center = plot.left + i * step + step / 2;
      const int currentX = center - pairGap / 2 - barWidth;
      const int previousX = center + (pairGap + 1) / 2;

      const auto drawBar = [&](double value, int x, COLORREF color, BYTE alpha) {
        if (!std::isfinite(value)) return;
        const int barHeight = static_cast<int>(usableHeight * value / maximum);
        RECT barRect{x, plot.bottom - barHeight, x + barWidth, plot.bottom};
        DrawWidgetCard(dc, barRect, color, 3, alpha);
        SetTextColor(dc, color);
        RECT valueRect{x - step / 5, barRect.top - valueBand,
                       x + barWidth + step / 5, barRect.top};
        DrawTextInRect(dc, NumberOrDash(value, value >= 10 ? 0 : 1), valueRect,
                       DT_CENTER | DT_SINGLELINE | DT_VCENTER);
      };

      drawBar(item.value, currentX, kWidgetCyan, 210);
      drawBar(item.previousWeekValue, previousX, kWidgetPurple, 120);

      SetTextColor(dc, kWidgetMuted);
      const std::wstring weekday = item.weekday.empty() ? L"-" : item.weekday;
      RECT weekdayRect{plot.left + i * step, plot.bottom,
                       plot.left + (i + 1) * step, chart.bottom};
      DrawTextInRect(dc, weekday, weekdayRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    SelectObject(dc, previousFont);
  }

  previousFont = SelectObject(dc, TierFont(FontTier::Small));
  SetTextColor(dc, kWidgetMuted);
  RECT plugRect{content.left, content.bottom - plugHeight, content.right, content.bottom};
  std::wstring plugs = L"Plug Mini情報なし";
  if (!nativeDashboard_.switchBotDevices.empty()) {
    plugs.clear();
    const size_t count = std::min<size_t>(2, nativeDashboard_.switchBotDevices.size());
    for (size_t i = 0; i < count; ++i) {
      if (i) plugs += L"  ";
      plugs += nativeDashboard_.switchBotDevices[i].name + L": " + nativeDashboard_.switchBotDevices[i].state;
    }
  }
  DrawTextInRect(dc, plugs, plugRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
  SelectObject(dc, previousFont);
}

void Renderer::DrawStationheadSection(HDC dc, const RECT& content) {
  const int64_t nowMs = UnixMillis();
  const NativePlaybackRender playbackA = ResolveNativePlayback(0, nowMs);
  const NativePlaybackRender playbackB = ResolveNativePlayback(1, nowMs);

  const auto drawRow = [&](int row, bool muted,
                           const NativePlaybackRender& playback,
                           const std::wstring& fallbackTitle, const std::wstring& detail) {
    const StationheadRowRects rects = StationheadRowFromSection(content, row);
    const RECT& rowRect = rects.row;

    const int rowHeight = rowRect.bottom - rowRect.top;
    const int pad = rowHeight * 14 / 100;
    const int artSize = rowHeight - pad * 2;
    RECT art{rowRect.left + pad, rowRect.top + pad, rowRect.left + pad + artSize,
             rowRect.top + pad + artSize};
    DrawWidgetCard(dc, art, kWidgetSurfaceAlt);
    if (playback.hasTrack) {
      DrawPremultipliedBitmap(dc,
                              NativeArtworkBitmap(playback.track.artwork, art.right - art.left,
                                                  art.bottom - art.top),
                              art);
    }

    const std::wstring title = playback.hasTrack ? playback.track.title : fallbackTitle;
    const std::wstring artist = playback.hasTrack ? playback.track.artist : L"";
    const bool withProgress = playback.hasTrack && playback.track.durationMs > 0;
    const int textLeft = art.right + pad;
    const int textRight = rects.button.left - pad;

    SetTextColor(dc, kWidgetText);
    HGDIOBJ previousFont = SelectObject(dc, TierFont(FontTier::Medium));
    RECT titleRect{textLeft, rowRect.top + rowHeight * 8 / 100, textRight,
                   rowRect.top + rowHeight * 44 / 100};
    DrawTextInRect(dc, title.empty() ? L"--" : title, titleRect,
                   DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

    SetTextColor(dc, kWidgetMuted);
    SelectObject(dc, TierFont(FontTier::Small));
    RECT artistRect{textLeft, titleRect.bottom, textRight,
                    withProgress ? rowRect.top + rowHeight * 64 / 100 : rowRect.bottom - pad};
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
        DrawWidgetPill(dc, fillRect, kWidgetGreen);
      }

      RECT timeRect{textLeft, barRect.bottom + rowHeight * 3 / 100,
                    textRight, rowRect.bottom - rowHeight * 3 / 100};
      DrawTextInRect(dc, TrackTimeText(playback.progressMs), timeRect,
                     DT_LEFT | DT_SINGLELINE | DT_VCENTER);
      DrawTextInRect(dc, TrackTimeText(playback.track.durationMs), timeRect,
                     DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }

    DrawWidgetCard(dc, rects.button, muted ? kWidgetDangerSurface : kWidgetSuccessSurface);
    SetTextColor(dc, muted ? kWidgetDanger : kWidgetGreen);
    SelectObject(dc, TierFont(FontTier::Small));
    DrawTextInRect(dc, muted ? L"音声OFF" : L"音声ON", rects.button,
                   DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, previousFont);
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


  const StationheadRowRects rowA = StationheadRowFromSection(content, 0);
  const StationheadRowRects rowB = StationheadRowFromSection(content, 1);
  DrawHorizontalDivider(dc, content, (rowA.row.bottom + rowB.row.top) / 2);
}

void Renderer::DrawClockControlsSection(HDC dc, const RECT& content) {
  SYSTEMTIME now{};
  GetLocalTime(&now);
  const ControlsButtonRects buttons = ControlsButtonsFromSection(content);
  const int dateHeight = SpanY(content, 160);
  const int timeHeight = SpanY(content, 400);
  const int blockTop = buttons.update.top - SpanY(content, 60) - timeHeight - dateHeight;

  RECT dateRect{content.left, blockTop, content.right, blockTop + dateHeight};
  HGDIOBJ previousFont = SelectObject(dc, TierFont(FontTier::Small));
  SetTextColor(dc, kWidgetMuted);
  DrawTextInRect(dc, DateText(now), dateRect, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_BOTTOM);

  RECT timeRect{content.left, dateRect.bottom, content.right, dateRect.bottom + timeHeight};
  SelectObject(dc, TierFont(FontTier::Large));
  SetTextColor(dc, kWidgetText);
  DrawTextInRect(dc, TimeText(now), timeRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

  SelectObject(dc, TierFont(FontTier::Small));
  SetTextColor(dc, kWidgetText);
  const std::array<std::pair<std::wstring, RECT>, 2> labels{{
      {L"更新", buttons.update},
      {L"再起動", buttons.restart},
  }};
  for (const auto& [label, rect] : labels) {
    DrawWidgetCard(dc, rect, kWidgetSurfaceAlt,  14,  170);
    DrawTextInRect(dc, label, rect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  }

  if (!nativeToast_.empty()) {
    SetTextColor(dc, kWidgetWarning);
    DrawTextInRect(dc, nativeToast_, buttons.status, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
  } else {
    std::wstring radarTime;
    bool hasFrame = false;
    {
      std::lock_guard lock(radarFrameMutex_);
      radarTime = radarTimeText_;
      hasFrame = radarFrameBitmap_ != nullptr;
    }
    SetTextColor(dc, kWidgetMuted);
    const std::wstring status = hasFrame
        ? L"レーダー更新: " + (radarTime.empty() ? L"--:--" : radarTime)
        : L"レーダーを準備中";
    DrawTextInRect(dc, status, buttons.status,
                   DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
  }
  SelectObject(dc, previousFont);
}

void Renderer::DrawWeatherSection(HDC dc, const RECT& content) {
  const auto& hours = nativeDashboard_.weatherHours;
  const size_t count = std::min<size_t>(5, hours.size());
  double maxPop = 0;
  for (size_t i = 0; i < count; ++i) {
    if (std::isfinite(hours[i].precipitationProbability)) maxPop = std::max(maxPop, hours[i].precipitationProbability);
  }

  const int contentHeight = std::max(1L, content.bottom - content.top);
  const int weatherHeight = contentHeight * 88 / 100;
  RECT weatherArea{content.left, content.top + (contentHeight - weatherHeight) / 2,
                   content.right, content.top + (contentHeight + weatherHeight) / 2};
  const int popWidth = SpanX(content, 240);
  RECT popRect{weatherArea.left, weatherArea.top, weatherArea.left + popWidth, weatherArea.bottom};

  HGDIOBJ previousFont = SelectObject(dc, TierFont(FontTier::Small));
  SetTextColor(dc, kWidgetMuted);
  RECT labelRect{popRect.left, popRect.top + SpanY(weatherArea, 80), popRect.right,
                 popRect.top + SpanY(weatherArea, 260)};
  DrawTextInRect(dc, L"降水確率", labelRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

  SelectObject(dc, TierFont(FontTier::Medium));
  SetTextColor(dc, maxPop >= 70 ? kWidgetBlue : maxPop >= 40 ? kWidgetBlueMuted : kWidgetMuted);
  RECT valueRect{popRect.left, labelRect.bottom, popRect.right, popRect.bottom - SpanY(weatherArea, 120)};
  DrawTextInRect(dc, std::to_wstring(static_cast<int>(std::round(maxPop))) + L"%", valueRect,
                 DT_CENTER | DT_SINGLELINE | DT_VCENTER);

  const int cardsLeft = popRect.right + SpanX(content, 40);
  DrawVerticalDivider(dc, weatherArea, (popRect.right + cardsLeft) / 2);
  const int cardGap = SpanX(content, 20);
  const int availableWidth = std::max(1L, weatherArea.right - cardsLeft);
  constexpr int kSlotCount = 5;
  const int cardWidth = std::max(1, (availableWidth - cardGap * (kSlotCount - 1)) / kSlotCount);
  for (int i = 0; i < kSlotCount; ++i) {
    RECT cardRect{cardsLeft + i * (cardWidth + cardGap), weatherArea.top,
                  cardsLeft + i * (cardWidth + cardGap) + cardWidth, weatherArea.bottom};
    SetTextColor(dc, kWidgetMuted);
    SelectObject(dc, TierFont(FontTier::Small));
    RECT hourRect{cardRect.left, cardRect.top + SpanY(weatherArea, 40), cardRect.right,
                  cardRect.top + SpanY(weatherArea, 200)};
    const std::wstring hour = i < static_cast<int>(count) ? std::to_wstring(hours[i].hour) + L"時" : L"--";
    DrawTextInRect(dc, hour, hourRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    if (i < static_cast<int>(count) && !hours[i].icon.empty()) {
      const int iconWidth = std::min(cardWidth, contentHeight * 63 / 100 * 90 / 48);
      const int iconHeight = std::max(12, iconWidth * 48 / 90);
      const int iconTop = hourRect.bottom + SpanY(weatherArea, 20);
      RECT iconRect{cardRect.left + (cardWidth - iconWidth) / 2, iconTop,
                    cardRect.left + (cardWidth + iconWidth) / 2, iconTop + iconHeight};
      DrawPremultipliedBitmap(dc,
                              NativeWeatherIconBitmap(hours[i].icon, IsWeatherNightHour(hours[i].hour),
                                                      iconWidth, iconHeight),
                              iconRect);
    }
    if (i < static_cast<int>(count) && std::isfinite(hours[i].temperature)) {
      SetTextColor(dc, kWidgetText);
      SelectObject(dc, TierFont(FontTier::Medium));
      RECT tempRect{cardRect.left, cardRect.bottom - SpanY(weatherArea, 380),
                    cardRect.right, cardRect.bottom - SpanY(weatherArea, 180)};
      DrawTextInRect(dc, NumberOrDash(hours[i].temperature, 0) + L"℃",
                     tempRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    SetTextColor(dc, kWidgetText);
    SelectObject(dc, TierFont(FontTier::Medium));
    RECT rainRect{cardRect.left, cardRect.bottom - SpanY(weatherArea, 170),
                  cardRect.right, cardRect.bottom - SpanY(weatherArea, 20)};
    const std::wstring rain = i < static_cast<int>(count) ? NumberOrDash(hours[i].rainMm, 0) + L"mm" : L"--";
    DrawTextInRect(dc, rain, rainRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
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
