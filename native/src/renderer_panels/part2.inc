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
  const int buttonHeight = std::min(
      std::clamp(rowHeight * 28 / 100, 20, 44),
      std::max(1, (rowHeight - buttonGap) / 2));
  const int buttonWidth = std::clamp(SpanX(body, 170), 64, 140);
  const int buttonTop = rects.row.top +
      (rowHeight - buttonHeight * 2 - buttonGap) / 2;
  rects.button = RECT{rects.row.right - buttonWidth, buttonTop, rects.row.right,
                      buttonTop + buttonHeight};
  rects.muteButton = RECT{rects.row.right - buttonWidth, buttonTop + buttonHeight + buttonGap,
                          rects.row.right, buttonTop + buttonHeight * 2 + buttonGap};
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
  const bool stationheadHistoryChanged = nativeStationheadPlayHistory_ != state.stationheadPlayHistory;
  const bool controlsChanged = nativeAppVersion_ != state.appVersion || nativeToast_ != state.toast;
  const bool newsChanged = nativeNewsIndex_ != state.newsIndex;
  const bool dashboardChanged = nativeRenderedDashboardRevision_ != dashboardSourceRevision_;

  if (sensorsChanged) nativeSensors_ = state.sensors;
  if (historyChanged) nativeAirHistory_ = state.airHistory;
  if (stationheadHistoryChanged) {
    nativeStationheadPlayHistory_ = state.stationheadPlayHistory;
  }
  if (stationheadChanged) nativeStationhead_ = state.stationhead;
  if (controlsChanged) {
    nativeAppVersion_ = state.appVersion;
    nativeToast_ = state.toast;
  }
  if (newsChanged) nativeNewsIndex_ = state.newsIndex;
  if (dashboardChanged) nativeRenderedDashboardRevision_ = dashboardSourceRevision_;
  if (!EnsureNativeStaticWindows()) return;
  if (sensorsChanged || historyChanged) InvalidatePanelSection(nativeSideWindow_, PanelSection::Air);
  if (dashboardChanged) {
    InvalidatePanelSection(nativeSideWindow_, PanelSection::Weather);
    InvalidatePanelSection(nativeMainWindow_, PanelSection::Energy);
  }
  if (dashboardChanged || newsChanged) InvalidatePanelSection(nativeMainWindow_, PanelSection::News);
  if (controlsChanged) InvalidatePanelSection(nativeSideWindow_, PanelSection::Controls);
  if (stationheadChanged || stationheadHistoryChanged) {
    InvalidatePanelSection(nativeSideWindow_, PanelSection::Clock);
  }
}

void Renderer::TickNativePanels(int64_t nowMs) {
  if (!nativeDashboardVisible_) return;
  const bool playbackActive = NativePlaybackActive(nowMs);
  if (nativeMainWindow_ && IsWindow(nativeMainWindow_) && IsWindowVisible(nativeMainWindow_)) {
    if (playbackActive) InvalidatePanelSection(nativeMainWindow_, PanelSection::Music);
  }
  if (nativeSideWindow_ && IsWindow(nativeSideWindow_) && IsWindowVisible(nativeSideWindow_) &&
      playbackActive) {
    InvalidatePanelSection(nativeSideWindow_, PanelSection::Clock);
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
        const RECT musicBody = CardBodyRect(CardContentRect(sections.clock));
        const MusicRowRects musicRects = MusicRowFromBody(musicBody);
        if (PtInRect(&musicRects.button, point)) {
          QueueAction(UiAction::StationheadAudioToggle);
          return 0;
        }
        if (PtInRect(&musicRects.muteButton, point)) {
          QueueAction(UiAction::StationheadAudioMute);
          return 0;
        }
        const ControlsRects controls = ControlsFromContent(CardContentRect(sections.controls));
        if (PtInRect(&controls.update, point)) QueueAction(UiAction::AppUpdate);
        else if (PtInRect(&controls.restart, point)) QueueAction(UiAction::Restart);
        return 0;
      }
      if (id == kNativeMainId) return 0;
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
    DrawMusicSection(scope.dc, sections.clock);
  }
  if (IntersectRect(&overlap, &scope.dirty, &sections.air)) {
    DrawAirSection(scope.dc, sections.air);
  }
  if (IntersectRect(&overlap, &scope.dirty, &sections.weather)) {
    DrawEnergySection(scope.dc, sections.weather);
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
    DrawClockSection(scope.dc, sections.music);
  }
  if (IntersectRect(&overlap, &scope.dirty, &sections.energy)) {
    DrawWeatherSection(scope.dc, sections.energy);
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
