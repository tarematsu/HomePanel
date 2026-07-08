#include "web_renderer.h"

namespace hp {
namespace {
constexpr UINT_PTR kClockTimerId = 1;
constexpr UINT kClockTimerMs = 1000;

RECT NativeClockRectFromBounds(const RECT& bounds) {
  const int clientWidth = std::max(1L, bounds.right - bounds.left);
  const int clientHeight = std::max(1L, bounds.bottom - bounds.top);
  const int dashboardWidth = std::min(static_cast<int>(clientWidth * 99LL / 100), 1660);
  const int dashboardHeight = std::min(static_cast<int>(clientHeight * 99LL / 100), 940);
  const int gap = 8;
  const int columnWidth = (dashboardWidth - gap * 2) / 3;
  const int rowHeight = (dashboardHeight - gap) / 2;
  const int dashboardLeft = bounds.left + (clientWidth - dashboardWidth) / 2;
  const int dashboardTop = bounds.top + (clientHeight - dashboardHeight) / 2;
  const int panelLeft = dashboardLeft + columnWidth + gap;
  const int panelTop = dashboardTop;

  RECT rect{
      panelLeft + 12,
      panelTop + 34,
      panelLeft + columnWidth - 12,
      panelTop + std::max(96, rowHeight - 94),
  };
  if (rect.right <= rect.left) rect.right = rect.left + 1;
  if (rect.bottom <= rect.top) rect.bottom = rect.top + 1;
  return rect;
}

HFONT CreateClockFont(int height, int weight) {
  return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, L"Yu Gothic UI");
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
  SetTimer(nativeClockWindow_, kClockTimerId, kClockTimerMs, nullptr);
  ApplyNativeClockBounds();
  return true;
}

void Renderer::ApplyNativeClockBounds() {
  if (!nativeClockWindow_ || !IsWindow(nativeClockWindow_)) return;
  const RECT rect = NativeClockRectFromBounds(bounds_);
  SetWindowPos(nativeClockWindow_, HWND_TOP, rect.left, rect.top,
               std::max(1L, rect.right - rect.left),
               std::max(1L, rect.bottom - rect.top), SWP_NOACTIVATE | SWP_SHOWWINDOW);
  InvalidateRect(nativeClockWindow_, nullptr, FALSE);
}

void Renderer::DestroyNativeClockWindow() {
  if (nativeClockWindow_ && IsWindow(nativeClockWindow_)) {
    KillTimer(nativeClockWindow_, kClockTimerId);
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

LRESULT Renderer::HandleNativeClockMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_ERASEBKGND:
      return 1;
    case WM_TIMER:
      if (wparam == kClockTimerId) {
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      break;
    case WM_PAINT:
      PaintNativeClock(hwnd);
      return 0;
    case WM_NCDESTROY:
      KillTimer(hwnd, kClockTimerId);
      if (nativeClockWindow_ == hwnd) nativeClockWindow_ = nullptr;
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
  HBITMAP bitmap = CreateCompatibleBitmap(dc, std::max(1L, bounds.right), std::max(1L, bounds.bottom));
  HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);
  HBRUSH background = CreateSolidBrush(RGB(9, 14, 21));
  FillRect(memoryDc, &bounds, background);
  DeleteObject(background);

  SetBkMode(memoryDc, TRANSPARENT);
  SetTextColor(memoryDc, RGB(238, 245, 255));

  SYSTEMTIME now{};
  GetLocalTime(&now);
  const int height = std::max(1L, bounds.bottom - bounds.top);
  const int dateHeight = std::clamp(height / 9, 14, 20);
  const int clockHeight = std::clamp(height / 3, 48, 72);

  RECT dateRect = bounds;
  dateRect.bottom = bounds.top + height / 2 - clockHeight / 2;
  HFONT dateFont = CreateClockFont(dateHeight, FW_NORMAL);
  HGDIOBJ previousFont = SelectObject(memoryDc, dateFont);
  DrawTextW(memoryDc, DateText(now).c_str(), -1, &dateRect,
            DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(memoryDc, previousFont);
  DeleteObject(dateFont);

  SetTextColor(memoryDc, RGB(255, 255, 255));
  RECT timeRect = bounds;
  timeRect.top = dateRect.bottom + 4;
  HFONT clockFont = CreateClockFont(clockHeight, FW_LIGHT);
  previousFont = SelectObject(memoryDc, clockFont);
  DrawTextW(memoryDc, TimeText(now).c_str(), -1, &timeRect,
            DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(memoryDc, previousFont);
  DeleteObject(clockFont);

  BitBlt(dc, 0, 0, bounds.right, bounds.bottom, memoryDc, 0, 0, SRCCOPY);
  SelectObject(memoryDc, previousBitmap);
  DeleteObject(bitmap);
  DeleteDC(memoryDc);
  EndPaint(hwnd, &paint);
}
}  // namespace hp
