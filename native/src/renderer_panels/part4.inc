void Renderer::DrawEnergySection(HDC dc, const RECT& card) {
  DrawSectionCard(dc, card);
  const RECT content = CardContentRect(card);
  HGDIOBJ previousFont = SelectObject(dc, TierFont(FontTier::Small));
  const RECT body = content;

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
      if (item.currentComplete && std::isfinite(item.currentTotal)) {
        maximum = std::max(maximum, item.currentTotal);
      }
      if (item.previousComplete && std::isfinite(item.previousTotal)) {
        maximum = std::max(maximum, item.previousTotal);
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
    const auto drawPreviousOverlay = [&](const RECT& rect) {
      AlphaBlendSolidColor(dc, rect, kWidgetBlack, 110);
      HPEN dottedPen = CreatePen(PS_DOT, 1, RGB(150, 150, 150));
      HGDIOBJ previousPen = SelectObject(dc, dottedPen);
      HGDIOBJ previousBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
      Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
      SelectObject(dc, previousBrush);
      SelectObject(dc, previousPen);
      DeleteObject(dottedPen);
    };
    RECT currentSwatch{legend.left, legend.top + (legendHeight - swatchHeight) / 2,
                       legend.left + swatchWidth,
                       legend.top + (legendHeight - swatchHeight) / 2 + swatchHeight};
    RECT previousSwatch{legend.left + legendHalf, currentSwatch.top,
                        legend.left + legendHalf + swatchWidth, currentSwatch.bottom};
    DrawWidgetCard(dc, currentSwatch, kWidgetCyan, 2, 220);
    drawPreviousOverlay(previousSwatch);

    std::wstring currentLegend = nativeDashboard_.currentEnergyLabel;
    std::wstring previousLegend = nativeDashboard_.previousEnergyLabel;
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
      const auto yFor = [&](double value) {
        const double ratio = std::clamp(value / maximum, 0.0, 1.0);
        return static_cast<int>(plot.bottom - (plot.bottom - plot.top) * ratio);
      };
      const int groupWidth = count > 0
          ? static_cast<int>((plot.right - plot.left) / static_cast<double>(count)) : 0;
      const int barGap = std::max(2, SpanX(body, 6));
      const int barWidth = std::max(3, groupWidth - barGap * 2);

      SelectObject(dc, TierFont(FontTier::Small));
      for (int index = 0; index < count; ++index) {
        const auto& point = nativeDashboard_.octopusProfile[static_cast<size_t>(index)];
        const int groupLeft = plot.left + index * groupWidth;
        const int currentLeft = groupLeft + barGap;

        if (point.currentComplete && std::isfinite(point.currentTotal)) {
          RECT bar{currentLeft, yFor(point.currentTotal), currentLeft + barWidth, plot.bottom};
          if (bar.bottom > bar.top) DrawWidgetCard(dc, bar, kWidgetCyan, 3, 220);
        }
        if (point.previousComplete && std::isfinite(point.previousTotal)) {
          RECT bar{currentLeft, yFor(point.previousTotal), currentLeft + barWidth, plot.bottom};
          if (bar.bottom > bar.top) drawPreviousOverlay(bar);
        }

        SetTextColor(dc, kWidgetSubtle);
        RECT dayLabel{groupLeft, plot.bottom, groupLeft + groupWidth, chart.bottom};
        DrawTextInRect(dc, point.day, dayLabel, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
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
  RECT titleRect{titleRow.left, titleRow.top, titleRow.right, titleRow.bottom};
  SetTextColor(dc, kWidgetText);
  SelectObject(dc, TierFont(FontTier::Medium));
  DrawTextInRect(dc, title, titleRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

  if (!description.empty()) {
    SetTextColor(dc, kWidgetMuted);
    SelectObject(dc, TierFont(FontTier::Small));
    RECT descPad{descRow.left, descRow.top + SpanY(content, 20), descRow.right, descRow.bottom};
    DrawTextInRect(dc, description, descPad, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
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
