namespace hp {
namespace {

thread_local RECT gDashboardClockCard{};

SidebarSections SplitRearrangedSidebarSections(const RECT& client) {
  const int height = std::max(1L, client.bottom - client.top);
  const int gap = std::max(6, height * 18 / 1000);
  const int available = std::max(4, height - gap * 3);
  const int clockHeight = available * 31 / 100;
  const int weatherHeight = available * 23 / 100;
  const int energyHeight = available * 23 / 100;
  const int airHeight = std::max(1, available - clockHeight - weatherHeight - energyHeight);

  SidebarSections sections;
  int top = client.top;
  sections.clock = RECT{client.left, top, client.right, top + clockHeight};
  top = sections.clock.bottom + gap;
  sections.weather = RECT{client.left, top, client.right, top + weatherHeight};
  top = sections.weather.bottom + gap;
  sections.energy = RECT{client.left, top, client.right, top + energyHeight};
  top = sections.energy.bottom + gap;
  sections.controls = RECT{client.left, top, client.right, top + airHeight};
  gDashboardClockCard = sections.clock;
  return sections;
}

MainSections SplitRearrangedMainSections(const RECT& client) {
  const int height = std::max(1L, client.bottom - client.top);
  const int gap = std::max(6, height * 45 / 1000);
  const int newsHeight = height * 47 / 100;

  MainSections sections;
  sections.news = RECT{client.left, client.bottom - newsHeight, client.right, client.bottom};
  sections.music = RECT{client.left, client.top, client.right,
                        std::max(client.top + 1, sections.news.top - gap)};
  sections.air = RECT{client.right, client.top, client.right + 1, client.top + 1};
  return sections;
}

RECT ClockControlsContent(const RECT& card) {
  const RECT content = CardContentRect(card);
  RECT controls{content.left, content.top + SpanY(content, 580), content.right, content.bottom};
  if (controls.bottom <= controls.top) controls.bottom = controls.top + 1;
  return controls;
}

ControlsRects RearrangedControlsFromContent(const RECT&) {
  return ControlsFromContent(ClockControlsContent(gDashboardClockCard));
}

#define HP_DRAW_CLOCK_WITH_CONTROLS(dc, card) do { \
  DrawSectionCard((dc), (card)); \
  const RECT hpClockContent = CardContentRect((card)); \
  SYSTEMTIME hpNow{}; GetLocalTime(&hpNow); \
  RECT hpDateRect{hpClockContent.left, hpClockContent.top, hpClockContent.right, hpClockContent.top + SpanY(hpClockContent, 180)}; \
  RECT hpTimeRect{hpClockContent.left, hpDateRect.bottom, hpClockContent.right, hpClockContent.top + SpanY(hpClockContent, 550)}; \
  HGDIOBJ hpPreviousFont = SelectObject((dc), TierFont(FontTier::Medium)); \
  SetTextColor((dc), kWidgetMuted); \
  DrawTextInRect((dc), DateText(hpNow), hpDateRect, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER); \
  SelectObject((dc), TierFont(FontTier::Large)); \
  SetTextColor((dc), kWidgetText); \
  DrawTextInRect((dc), TimeText(hpNow), hpTimeRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER); \
  const ControlsRects hpControls = RearrangedControlsFromContent((card)); \
  SelectObject((dc), TierFont(FontTier::Small)); \
  DrawWidgetCard((dc), hpControls.update, kWidgetSurfaceAlt, CardRadius(hpControls.update)); \
  DrawWidgetCard((dc), hpControls.restart, kWidgetSurfaceAlt, CardRadius(hpControls.restart)); \
  SetTextColor((dc), kWidgetText); \
  DrawTextInRect((dc), L"更新", hpControls.update, DT_CENTER | DT_SINGLELINE | DT_VCENTER); \
  DrawTextInRect((dc), L"再起動", hpControls.restart, DT_CENTER | DT_SINGLELINE | DT_VCENTER); \
  if (!nativeToast_.empty()) { \
    SetTextColor((dc), kWidgetWarning); \
    DrawTextInRect((dc), nativeToast_, hpControls.status, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS); \
  } else { \
    std::wstring hpRadarTime; \
    bool hpHasFrame = false; \
    { std::lock_guard hpLock(radarFrameMutex_); hpRadarTime = radarTimeText_; hpHasFrame = radarFrameBitmap_ != nullptr; } \
    const std::wstring hpStatus = hpHasFrame ? L"レーダー更新: " + (hpRadarTime.empty() ? L"--:--" : hpRadarTime) : L"レーダーを準備中"; \
    const int hpStatusHeight = static_cast<int>(hpControls.status.bottom - hpControls.status.top); \
    RECT hpFirstLine = hpControls.status; hpFirstLine.bottom = hpControls.status.top + hpStatusHeight / 2; \
    RECT hpSecondLine = hpControls.status; hpSecondLine.top = hpFirstLine.bottom; \
    SetTextColor((dc), kWidgetMuted); \
    DrawTextInRect((dc), hpStatus, hpFirstLine, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER); \
    if (!nativeAppVersion_.empty()) { \
      SetTextColor((dc), kWidgetSubtle); \
      DrawTextInRect((dc), L"バージョン " + nativeAppVersion_, hpSecondLine, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER); \
    } \
  } \
  SelectObject((dc), hpPreviousFont); \
} while (false)

}  // namespace
}  // namespace hp
