namespace {

SidebarSections SplitRearrangedSidebarSections(const RECT& client) {
  const int height = std::max(1L, client.bottom - client.top);
  const int gap = std::max(6, height * 18 / 1000);
  const int available = std::max(3, height - gap * 2);
  const int clockHeight = available * 27 / 100;
  const int remainingHeight = std::max(2, available - clockHeight);
  const int weatherHeight = remainingHeight / 2;
  const int airHeight = std::max(1, remainingHeight - weatherHeight);

  SidebarSections sections;
  int top = client.top;
  sections.clock = RECT{client.left, top, client.right, top + clockHeight};
  top = sections.clock.bottom + gap;
  sections.weather = RECT{client.left, top, client.right, top + weatherHeight};
  top = sections.weather.bottom + gap;
  sections.controls = RECT{client.left, top, client.right, top + airHeight};
  // Electricity moved beside Music in the main column. Keep the legacy slot
  // outside the side client so old section plumbing cannot paint it twice.
  sections.energy = RECT{client.right, client.bottom, client.right + 1, client.bottom + 1};
  return sections;
}

MainSections SplitRearrangedMainSections(const RECT& client) {
  const int width = std::max(1L, client.right - client.left);
  const int height = std::max(1L, client.bottom - client.top);
  const int gapX = std::max(8, width * 16 / 1000);
  const int gapY = std::max(6, height * 45 / 1000);
  const int newsHeight = height * 47 / 100;
  const int rowWidth = std::max(2, width - gapX);
  const int musicWidth = std::max(1, rowWidth * 65 / 100);

  MainSections sections;
  sections.news = RECT{client.left, client.bottom - newsHeight, client.right, client.bottom};
  const LONG rowBottom = std::max(client.top + 1, sections.news.top - gapY);
  sections.music = RECT{client.left, client.top, client.left + musicWidth, rowBottom};
  // MainSections::air is the right-hand top-row slot. The rearranged dashboard
  // uses it for Electricity while Air occupies the expanded side-card slot.
  sections.air = RECT{sections.music.right + gapX, client.top, client.right, rowBottom};
  if (sections.air.right <= sections.air.left) {
    sections.air.right = sections.air.left + 1;
  }
  return sections;
}

RECT RearrangedClockTimeRectFromCard(const RECT& card) {
  const RECT content = CardContentRect(card);
  // Include the footer in the clock tick invalidation so the moved status is
  // refreshed without a separate polling or invalidation subsystem.
  RECT dynamicRect{content.left, content.top + SpanY(content, 170),
                   content.right, content.bottom};
  if (dynamicRect.right <= dynamicRect.left) dynamicRect.right = dynamicRect.left + 1;
  if (dynamicRect.bottom <= dynamicRect.top) dynamicRect.bottom = dynamicRect.top + 1;
  return dynamicRect;
}

std::wstring DashboardStatusTimeText(int64_t epochMs) {
  if (epochMs <= 0) return L"--:--";
  const std::time_t seconds = static_cast<std::time_t>(epochMs / 1000);
  std::tm local{};
  if (localtime_s(&local, &seconds) != 0) return L"--:--";
  wchar_t text[16]{};
  if (wcsftime(text, std::size(text), L"%H:%M", &local) == 0) return L"--:--";
  return text;
}

#define HP_DRAW_CLOCK_WITH_STATUS(dc, card) do { \
  DrawSectionCard((dc), (card)); \
  const RECT hpClockContent = CardContentRect((card)); \
  SYSTEMTIME hpNow{}; GetLocalTime(&hpNow); \
  RECT hpDateRect{hpClockContent.left, hpClockContent.top, hpClockContent.right, hpClockContent.top + SpanY(hpClockContent, 170)}; \
  RECT hpTimeRect{hpClockContent.left, hpDateRect.bottom, hpClockContent.right, hpClockContent.top + SpanY(hpClockContent, 705)}; \
  RECT hpStatusRect{hpClockContent.left, hpClockContent.top + SpanY(hpClockContent, 735), hpClockContent.right, hpClockContent.bottom}; \
  HGDIOBJ hpPreviousFont = SelectObject((dc), TierFont(FontTier::Medium)); \
  SetTextColor((dc), kWidgetMuted); \
  DrawTextInRect((dc), DateText(hpNow), hpDateRect, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER); \
  const int hpTimeHeight = std::max(1L, hpTimeRect.bottom - hpTimeRect.top); \
  const int hpTimeWidth = std::max(1L, hpTimeRect.right - hpTimeRect.left); \
  const int hpTimeFontHeight = std::clamp(std::min(hpTimeHeight * 72 / 100, hpTimeWidth * 18 / 100), 32, 88); \
  SelectObject((dc), CachedUiFont(hpTimeFontHeight, FW_SEMIBOLD)); \
  SetTextColor((dc), kWidgetText); \
  DrawTextInRect((dc), TimeText(hpNow), hpTimeRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER); \
  const NativeMinuteFactsProjection hpStationheadStatus = NativeMinuteFactsSnapshot(); \
  std::wstring hpStateText = L"状態取得待ち"; \
  if (hpStationheadStatus.available) { \
    if (hpStationheadStatus.isPaused) hpStateText = L"一時停止"; \
    else if (hpStationheadStatus.isBroadcasting) hpStateText = L"配信中 " + std::to_wstring(hpStationheadStatus.listenerCount); \
    else hpStateText = L"停止中"; \
  } \
  const std::wstring hpStatusTime = L"生成" + DashboardStatusTimeText(hpStationheadStatus.minuteAt) + L" 取得" + DashboardStatusTimeText(hpStationheadStatus.fetchedAt); \
  const LONG hpStatusMiddle = hpStatusRect.top + std::max<LONG>(1, (hpStatusRect.bottom - hpStatusRect.top) / 2); \
  RECT hpStateRect = hpStatusRect; hpStateRect.bottom = hpStatusMiddle; \
  RECT hpStatusTimeRect = hpStatusRect; hpStatusTimeRect.top = hpStatusMiddle; \
  SelectObject((dc), TierFont(FontTier::Small)); \
  SetTextColor((dc), hpStationheadStatus.available ? kWidgetMuted : kWidgetSubtle); \
  DrawTextInRect((dc), hpStateText, hpStateRect, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER); \
  DrawTextInRect((dc), hpStatusTime, hpStatusTimeRect, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER); \
  SelectObject((dc), hpPreviousFont); \
} while (false)

}  // namespace
