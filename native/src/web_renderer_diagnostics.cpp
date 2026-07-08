#include "web_renderer.h"

namespace hp {
namespace {
constexpr int64_t kDiagnosticRepeatSuppressMs = 60'000;

std::wstring HResultText(HRESULT result) {
  std::wostringstream output;
  output << L"0x" << std::uppercase << std::hex << std::setw(8) << std::setfill(L'0')
         << static_cast<unsigned long>(result);
  return output.str();
}

std::wstring TimeText() {
  SYSTEMTIME now{};
  GetLocalTime(&now);
  wchar_t text[64]{};
  swprintf_s(text, L"%04u-%02u-%02u %02u:%02u:%02u",
             now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
  return text;
}
}

void Renderer::AppendWebViewDiagnostic(const std::wstring& message) const {
  std::lock_guard lock(diagnosticMutex_);
  const int64_t now = UnixMillis();
  if (message == lastDiagnosticMessage_ &&
      now - lastDiagnosticLoggedAt_ < kDiagnosticRepeatSuppressMs) {
    return;
  }
  if (!diagnosticDirectoryEnsured_) {
    std::error_code ignored;
    fs::create_directories(dataDir_, ignored);
    diagnosticDirectoryEnsured_ = true;
  }
  std::error_code ignored;
  std::wofstream log(dataDir_ / L"webview2-dashboard.log", std::ios::app);
  if (!log) return;
  log << L"[" << TimeText() << L"] " << message;
  if (!runtimeVersion_.empty()) log << L" | runtime=" << runtimeVersion_;
  log << L"\n";
  lastDiagnosticMessage_ = message;
  lastDiagnosticLoggedAt_ = now;
}

void Renderer::SetWebViewError(const std::wstring& stage, HRESULT result) {
  webViewError_ = stage + L" failed (" + HResultText(result) + L")";
  AppendWebViewDiagnostic(webViewError_);
  creating_ = false;
  if (window_) InvalidateRect(window_, nullptr, FALSE);
}

void Renderer::DrawStartupFallback(const RECT&) const {
  if (!window_) return;
  HDC dc = GetDC(window_);
  if (!dc) return;
  RECT bounds{};
  GetClientRect(window_, &bounds);
  FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(242, 245, 247));
  HFONT font = CreateFontW(
      -42, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_DONTCARE, L"Yu Gothic UI");
  HFONT previous = static_cast<HFONT>(SelectObject(dc, font));
  DrawTextW(dc, L"起動中", -1, &bounds,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(dc, previous);
  DeleteObject(font);
  ReleaseDC(window_, dc);
}
}  // namespace hp
