from pathlib import Path

path = Path("native/src/renderer_panels.cpp")
text = path.read_text(encoding="utf-8")
start = "  if (chart.bottom > chart.top + 20 && !nativeDashboard_.octopusHistory.empty()) {"
end = "\n\n  previousFont = SelectObject(dc, TierFont(FontTier::Small));"
start_index = text.find(start)
if start_index < 0:
    if "nativeDashboard_.octopusProfile.empty()" in text:
        raise SystemExit(0)
    raise SystemExit("energy bar chart start marker not found")
end_index = text.find(end, start_index)
if end_index < 0:
    raise SystemExit("energy bar chart end marker not found")

replacement = r'''  if (chart.bottom > chart.top + 20 && !nativeDashboard_.octopusProfile.empty()) {
    double maximum = 0.1;
    for (const auto& item : nativeDashboard_.octopusProfile) {
      if (std::isfinite(item.currentAverage)) maximum = std::max(maximum, item.currentAverage);
      if (std::isfinite(item.previousAverage)) maximum = std::max(maximum, item.previousAverage);
    }
    maximum *= 1.1;

    const int legendHeight = SpanY(content, 125);
    const int axisHeight = SpanY(content, 85);
    const int yLabelWidth = SpanX(content, 90);
    RECT legend{chart.left, chart.top, chart.right, chart.top + legendHeight};
    previousFont = SelectObject(dc, TierFont(FontTier::Small));

    const int legendHalf = (legend.right - legend.left) / 2;
    const int swatchWidth = std::max(18, SpanX(content, 55));
    const int swatchHeight = std::max(3, SpanY(content, 10));
    RECT currentSwatch{legend.left, legend.top + legendHeight / 3,
                       legend.left + swatchWidth, legend.top + legendHeight / 3 + swatchHeight};
    RECT previousSwatch{legend.left + legendHalf, currentSwatch.top,
                        legend.left + legendHalf + swatchWidth, currentSwatch.bottom};
    DrawWidgetCard(dc, currentSwatch, kWidgetCyan, 2, 220);
    DrawWidgetCard(dc, previousSwatch, kWidgetPurple, 2, 120);

    std::wstring currentLegend = nativeDashboard_.currentEnergyLabel;
    if (!nativeDashboard_.currentEnergyDateRange.empty()) {
      currentLegend += L" " + nativeDashboard_.currentEnergyDateRange;
    }
    std::wstring previousLegend = nativeDashboard_.previousEnergyLabel;
    if (!nativeDashboard_.previousEnergyDateRange.empty()) {
      previousLegend += L" " + nativeDashboard_.previousEnergyDateRange;
    }
    SetTextColor(dc, kWidgetMuted);
    RECT currentLegendRect{currentSwatch.right + SpanX(content, 15), legend.top,
                           legend.left + legendHalf - SpanX(content, 10), legend.bottom};
    RECT previousLegendRect{previousSwatch.right + SpanX(content, 15), legend.top,
                            legend.right, legend.bottom};
    DrawTextInRect(dc, currentLegend, currentLegendRect,
                   DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    DrawTextInRect(dc, previousLegend, previousLegendRect,
                   DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    RECT plot{chart.left + yLabelWidth, legend.bottom,
              chart.right - SpanX(content, 10), chart.bottom - axisHeight};
    if (plot.right > plot.left + 20 && plot.bottom > plot.top + 20) {
      for (int line = 0; line <= 4; ++line) {
        const int y = plot.bottom - static_cast<int>((plot.bottom - plot.top) * line / 4.0);
        RECT grid{plot.left, y, plot.right, y + 1};
        AlphaBlendSolidColor(dc, grid, RGB(255, 255, 255), line == 0 ? 70 : 35);
      }

      SetTextColor(dc, kWidgetSubtle);
      RECT maxLabel{chart.left, plot.top - SpanY(content, 20),
                    plot.left - SpanX(content, 10), plot.top + SpanY(content, 50)};
      DrawTextInRect(dc, Fixed(maximum, maximum >= 1.0 ? 1 : 2), maxLabel,
                     DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
      RECT zeroLabel{chart.left, plot.bottom - SpanY(content, 35),
                     plot.left - SpanX(content, 10), plot.bottom + SpanY(content, 35)};
      DrawTextInRect(dc, L"0", zeroLabel, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

      const int count = static_cast<int>(nativeDashboard_.octopusProfile.size());
      const auto xFor = [&](int index) {
        return count <= 1 ? static_cast<int>(plot.left)
            : static_cast<int>(plot.left + (plot.right - plot.left) * index /
                static_cast<double>(count - 1));
      };
      const auto yFor = [&](double value) {
        const double ratio = std::clamp(value / maximum, 0.0, 1.0);
        return static_cast<int>(plot.bottom - (plot.bottom - plot.top) * ratio);
      };
      const auto blendOnBackground = [](COLORREF foreground, BYTE alpha) {
        const int inverse = 255 - alpha;
        const int red = (GetRValue(foreground) * alpha + GetRValue(kWidgetBackground) * inverse) / 255;
        const int green = (GetGValue(foreground) * alpha + GetGValue(kWidgetBackground) * inverse) / 255;
        const int blue = (GetBValue(foreground) * alpha + GetBValue(kWidgetBackground) * inverse) / 255;
        return RGB(red, green, blue);
      };
      const auto drawSeries = [&](bool current, COLORREF color, int width) {
        HPEN pen = CreatePen(PS_SOLID, width, color);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        bool started = false;
        for (int index = 0; index < count; ++index) {
          const auto& point = nativeDashboard_.octopusProfile[index];
          const double value = current ? point.currentAverage : point.previousAverage;
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
          {0, L"0:00"}, {12, L"6:00"}, {24, L"12:00"}, {36, L"18:00"}, {47, L"24:00"},
      }};
      SetTextColor(dc, kWidgetSubtle);
      for (const auto& [index, label] : ticks) {
        const int x = xFor(index);
        RECT tick{x - SpanX(content, 55), plot.bottom,
                  x + SpanX(content, 55), chart.bottom};
        DrawTextInRect(dc, label, tick, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
      }
      RECT unit{chart.left, plot.top, plot.left - SpanX(content, 10),
                plot.top + SpanY(content, 55)};
      DrawTextInRect(dc, L"kWh/30分", unit, DT_RIGHT | DT_SINGLELINE | DT_TOP);
    }
    SelectObject(dc, previousFont);
  }'''

path.write_text(text[:start_index] + replacement + text[end_index:], encoding="utf-8")
