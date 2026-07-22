HPEN CachedAirDottedPen(COLORREF color) {
  static std::map<COLORREF, HPEN> cache;
  auto [entry, inserted] = cache.try_emplace(color, nullptr);
  if (inserted) entry->second = CreatePen(PS_DOT, 1, color);
  return entry->second;
}

template <typename ValueOf>
void DrawDottedAirHistoryLine(
    HDC dc, const std::vector<AirHistorySample>& samples, const RECT& plot,
    int64_t cutoff, int64_t spanMs, double minValue, double maxValue,
    COLORREF color, ValueOf&& valueOf) {
  if (samples.empty() || maxValue <= minValue) return;
  HGDIOBJ previousPen = SelectObject(dc, CachedAirDottedPen(color));
  bool started = false;
  for (const auto& sample : samples) {
    const double value = valueOf(sample);
    if (!std::isfinite(value) || sample.timestamp < cutoff) continue;
    const double xRatio = std::clamp(
        static_cast<double>(sample.timestamp - cutoff) / spanMs, 0.0, 1.0);
    const double yRatio = std::clamp(
        (value - minValue) / (maxValue - minValue), 0.0, 1.0);
    const int x = plot.left +
        static_cast<int>((plot.right - plot.left) * xRatio);
    const int y = plot.bottom -
        static_cast<int>((plot.bottom - plot.top) * yRatio);
    if (!started) {
      MoveToEx(dc, x, y, nullptr);
      started = true;
    } else {
      LineTo(dc, x, y);
    }
  }
  SelectObject(dc, previousPen);
}

void Renderer::DrawClockSection(HDC dc, const RECT& card) {
  DrawSectionCard(dc, card);
  const RECT content = CardContentRect(card);
  SYSTEMTIME now{};
  GetLocalTime(&now);

  RECT dateRect{content.left, content.top, content.right, content.top + SpanY(content, 320)};
  HGDIOBJ previousFont = SelectObject(dc, TierFont(FontTier::Medium));
  SetTextColor(dc, kWidgetMuted);
  DrawTextInRect(dc, DateText(now), dateRect,
                 DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_BOTTOM);

  RECT timeRect{content.left, dateRect.bottom, content.right, content.bottom};
  SelectObject(dc, TierFont(FontTier::Large));
  SetTextColor(dc, kWidgetText);
  DrawTextInRect(dc, TimeText(now), timeRect, DT_CENTER | DT_SINGLELINE | DT_TOP);
  SelectObject(dc, previousFont);
}

void Renderer::DrawAirSection(HDC dc, const RECT& card) {
  DrawSectionCard(dc, card);
  const RECT content = CardContentRect(card);
  HGDIOBJ previousFont = SelectObject(dc, TierFont(FontTier::Small));
  const RECT body = content;

  const bool connected = nativeSensors_.co2Connected;
  struct AirStat {
    std::wstring label;
    std::wstring value;
    COLORREF color;
  };
  const std::array<AirStat, 3> stats{{
      {L"CO2", connected ? std::to_wstring(nativeSensors_.co2) + L" ppm" : L"--- ppm",
       connected ? kWidgetGreen : kWidgetMuted},
      {L"温度", connected ? Fixed(nativeSensors_.temperatureCorrected, 1) + L"℃" : L"--.-℃",
       connected ? kWidgetText : kWidgetMuted},
      {L"湿度", connected ? Fixed(nativeSensors_.humidityCorrected, 0) + L"%" : L"--%",
       kWidgetMuted},
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

  constexpr int64_t kWindowMs = 24LL * 60 * 60 * 1000;
  const auto& samples = nativeAirGraph_.samples;
  const int64_t cutoff = nativeAirGraph_.cutoff;

  const int plotTop = body.top + statsHeight + SpanY(body, 60);
  const RECT plot{body.left, plotTop, body.right, body.bottom};
  if (plot.right > plot.left + 4 && plot.bottom > plot.top + 4) {
    HPEN gridPen = CreatePen(PS_SOLID, 1, kWidgetTrack);
    HGDIOBJ previousPen = SelectObject(dc, gridPen);
    for (int i = 0; i < 4; ++i) {
      const int y = plot.top + (plot.bottom - plot.top) * i / 3;
      MoveToEx(dc, plot.left, y, nullptr);
      LineTo(dc, plot.right, y);
    }
    for (int i = 1; i <= 3; ++i) {
      const int x = plot.left + (plot.right - plot.left) * i / 4;
      MoveToEx(dc, x, plot.top, nullptr);
      LineTo(dc, x, plot.bottom);
    }
    SelectObject(dc, previousPen);
    DeleteObject(gridPen);
  }

  if (!samples.empty() && plot.bottom > plot.top + 4) {
    DrawHistoryLine(dc, samples, plot, cutoff, kWindowMs,
                    nativeAirGraph_.co2Min - 80, nativeAirGraph_.co2Max + 80,
                    kWidgetGreen, 2,
                    [](const AirHistorySample& s) { return static_cast<double>(s.co2); });
    DrawHistoryLine(
        dc, samples, plot, cutoff, kWindowMs,
        nativeAirGraph_.temperatureMin - 0.5,
        nativeAirGraph_.temperatureMax + 0.5,
        kWidgetText, 1,
        [](const AirHistorySample& s) { return s.temperature; });
    DrawDottedAirHistoryLine(
        dc, samples, plot, cutoff, kWindowMs,
        nativeAirGraph_.humidityMin - 2,
        nativeAirGraph_.humidityMax + 2,
        kWidgetMuted,
        [](const AirHistorySample& s) { return s.humidity; });
  } else {
    SetTextColor(dc, kWidgetSubtle);
    DrawTextInRect(dc, L"履歴を取得中", plot, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  }

  SelectObject(dc, previousFont);
}

void Renderer::DrawWeatherSection(HDC dc, const RECT& card) {
  DrawSectionCard(dc, card);
  const RECT content = CardContentRect(card);
  const auto& hours = nativeDashboard_.weatherHours;
  constexpr int kColumnCount = 6;
  constexpr int kRowCount = 2;
  constexpr int kSlotCount = kColumnCount * kRowCount;
  static constexpr std::array<int, kSlotCount> kWeatherHourOrder{
      22, 23, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  std::array<const WeatherHourData*, kSlotCount> slotHours{};
  for (const auto& hour : hours) {
    const auto slot = std::find(kWeatherHourOrder.begin(), kWeatherHourOrder.end(), hour.hour);
    if (slot != kWeatherHourOrder.end()) {
      slotHours[static_cast<size_t>(std::distance(kWeatherHourOrder.begin(), slot))] = &hour;
    }
  }

  HGDIOBJ previousFont = SelectObject(dc, TierFont(FontTier::Small));
  const RECT body = content;
  const int bodyWidth = std::max(1, static_cast<int>(body.right - body.left));
  const int bodyHeight = std::max(1, static_cast<int>(body.bottom - body.top));
  const int columnGap = std::max(3, SpanX(body, 18));
  const int rowGap = std::max(4, SpanY(body, 20));
  const int columnWidth = std::max(1, (bodyWidth - columnGap * (kColumnCount - 1)) / kColumnCount);
  const int rowHeight = std::max(1, (bodyHeight - rowGap) / kRowCount);

  for (int i = 0; i < kSlotCount; ++i) {
    const int row = i / kColumnCount;
    const int columnIndex = i % kColumnCount;
    const int left = body.left + columnIndex * (columnWidth + columnGap);
    const int top = body.top + row * (rowHeight + rowGap);
    const RECT column{
        left,
        top,
        columnIndex == kColumnCount - 1 ? body.right : left + columnWidth,
        row == kRowCount - 1 ? body.bottom : top + rowHeight,
    };
    const WeatherHourData* hourData = slotHours[static_cast<size_t>(i)];
    if (!hourData) continue;
    const int columnHeight = std::max(1, static_cast<int>(column.bottom - column.top));
    const bool hasIcon = !hourData->icon.empty();

    const int hourHeight = std::max(1, columnHeight * 18 / 100);
    const int iconSlotHeight = std::max(1, columnHeight * 42 / 100);
    const int temperatureHeight = std::max(1, columnHeight * 22 / 100);

    SetTextColor(dc, kWidgetMuted);
    SelectObject(dc, TierFont(FontTier::Small));
    RECT hourRect{column.left, column.top, column.right, column.top + hourHeight};
    const std::wstring hour = std::to_wstring(hourData->hour) + L"時";
    DrawTextInRect(dc, hour, hourRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    RECT iconSlot{column.left, hourRect.bottom, column.right,
                  std::min(static_cast<int>(column.bottom),
                           static_cast<int>(hourRect.bottom) + iconSlotHeight)};
    if (hasIcon && iconSlot.bottom > iconSlot.top) {
      int iconWidth = std::min(static_cast<int>(column.right - column.left),
                               iconSlotHeight * 90 / 48);
      iconWidth = std::max(1, iconWidth);
      const int iconHeight = std::max(1, std::min(iconSlotHeight, iconWidth * 48 / 90));
      iconWidth = std::max(1, std::min(iconWidth, iconHeight * 90 / 48));
      RECT iconRect{
          column.left + (static_cast<int>(column.right - column.left) - iconWidth) / 2,
          iconSlot.top + (static_cast<int>(iconSlot.bottom - iconSlot.top) - iconHeight) / 2,
          column.left + (static_cast<int>(column.right - column.left) + iconWidth) / 2,
          iconSlot.top + (static_cast<int>(iconSlot.bottom - iconSlot.top) + iconHeight) / 2,
      };
      DrawPremultipliedBitmap(dc,
          NativeWeatherIconBitmap(hourData->icon,
                                  IsWeatherNightHour(hourData->hour),
                                  iconWidth, iconHeight),
          iconRect);
    }

    SetTextColor(dc, kWidgetText);
    SelectObject(dc, TierFont(FontTier::Medium));
    RECT tempRect{column.left, iconSlot.bottom, column.right,
                  std::min(static_cast<int>(column.bottom),
                           static_cast<int>(iconSlot.bottom) + temperatureHeight)};
    const std::wstring temp = NumberOrDash(hourData->temperature, 0) + L"℃";
    DrawTextInRect(dc, temp, tempRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    const double rainMm = hourData->rainMm;
    SetTextColor(dc, std::isfinite(rainMm) && rainMm > 0 ? kWidgetBlueMuted : kWidgetSubtle);
    SelectObject(dc, TierFont(FontTier::Small));
    RECT rainRect{column.left, tempRect.bottom, column.right, column.bottom};
    const std::wstring rain = NumberOrDash(rainMm, 0) + L"mm";
    DrawTextInRect(dc, rain, rainRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  }
  SelectObject(dc, previousFont);
}
