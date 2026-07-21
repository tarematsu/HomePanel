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

HBRUSH CachedSolidBrush(COLORREF color) { static std::map<COLORREF, HBRUSH> cache; auto [entry, inserted] = cache.try_emplace(color, nullptr); if (inserted) entry->second = CreateSolidBrush(color); return entry->second; }
HPEN CachedSolidPen(int width, COLORREF color) { static std::map<std::pair<int, COLORREF>, HPEN> cache; const auto key = std::make_pair(width, color); auto [entry, inserted] = cache.try_emplace(key, nullptr); if (inserted) entry->second = CreatePen(PS_SOLID, width, color); return entry->second; }
