}

void Renderer::DrawClockSection(HDC dc, const RECT& card) {
  DrawSectionCard(dc, card);
  const RECT content = CardContentRect(card);
  SYSTEMTIME now{};
  GetLocalTime(&now);

  RECT dateRect{content.left, content.top, content.right, content.top + SpanY(content, 320)};
  HGDIOBJ previousFont = SelectObject(dc, TierFont(FontTier::Medium));
  SetTextColor(dc, kWidgetMuted);
  DrawTextInRect(dc, DateText(now), dateRect, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

  RECT timeRect{content.left, dateRect.bottom, content.right, content.bottom};
  SelectObject(dc, TierFont(FontTier::Large));
  SetTextColor(dc, kWidgetText);
  DrawTextInRect(dc, TimeText(now), timeRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
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
       connected ? kWidgetOrange : kWidgetMuted},
      {L"湿度", connected ? Fixed(nativeSensors_.humidityCorrected, 0) + L"%" : L"--%",
       connected ? kWidgetBlueMuted : kWidgetMuted},
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

  const int64_t now = UnixMillis();
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
    DrawHistoryLine(dc, samples, plot, cutoff, kWindowMs, co2Min - 80, co2Max + 80, kWidgetGreen, 2,
                    [](const AirHistorySample& s) { return static_cast<double>(s.co2); });
    DrawHistoryLine(dc, samples, plot, cutoff, kWindowMs, temperatureMin - 0.5, temperatureMax + 0.5,
                    kWidgetOrange, 1,
                    [](const AirHistorySample& s) { return s.temperature; });
    DrawHistoryLine(dc, samples, plot, cutoff, kWindowMs, humidityMin - 2, humidityMax + 2,
                    kWidgetBlueMuted, 1,
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
  const size_t count = std::min<size_t>(kSlotCount, hours.size());

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
    const int columnHeight = std::max(1, static_cast<int>(column.bottom - column.top));
    const bool hasData = i < static_cast<int>(count);
    const bool hasIcon = hasData && !hours[static_cast<size_t>(i)].icon.empty();

    const int hourHeight = std::max(1, columnHeight * 18 / 100);
    const int iconSlotHeight = std::max(1, columnHeight * 42 / 100);
    const int temperatureHeight = std::max(1, columnHeight * 22 / 100);

    SetTextColor(dc, kWidgetMuted);
    SelectObject(dc, TierFont(FontTier::Small));
    RECT hourRect{column.left, column.top, column.right, column.top + hourHeight};
    const std::wstring hour = hasData
        ? std::to_wstring(hours[static_cast<size_t>(i)].hour) + L"時" : L"--";
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
          NativeWeatherIconBitmap(hours[static_cast<size_t>(i)].icon,
                                  IsWeatherNightHour(hours[static_cast<size_t>(i)].hour),
                                  iconWidth, iconHeight),
          iconRect);
    }

    SetTextColor(dc, kWidgetText);
    SelectObject(dc, TierFont(FontTier::Medium));
    RECT tempRect{column.left, iconSlot.bottom, column.right,
                  std::min(static_cast<int>(column.bottom),
                           static_cast<int>(iconSlot.bottom) + temperatureHeight)};
    const std::wstring temp = hasData
        ? NumberOrDash(hours[static_cast<size_t>(i)].temperature, 0) + L"℃" : L"--";
    DrawTextInRect(dc, temp, tempRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    const double rainMm = hasData ? hours[static_cast<size_t>(i)].rainMm
                                  : std::numeric_limits<double>::quiet_NaN();
    SetTextColor(dc, std::isfinite(rainMm) && rainMm > 0 ? kWidgetBlueMuted : kWidgetSubtle);
    SelectObject(dc, TierFont(FontTier::Small));
    RECT rainRect{column.left, tempRect.bottom, column.right, column.bottom};
    const std::wstring rain = hasData ? NumberOrDash(rainMm, 0) + L"mm" : L"--";
    DrawTextInRect(dc, rain, rainRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  }
  SelectObject(dc, previousFont);
}

void Renderer::DrawControlsSection(HDC dc, const RECT& card) {
  DrawSectionCard(dc, card);
  const RECT content = CardContentRect(card);
  const ControlsRects rects = ControlsFromContent(content);

  HGDIOBJ previousFont = SelectObject(dc, TierFont(FontTier::Small));
  const std::array<std::pair<std::wstring, RECT>, 2> buttons{{
      {L"更新", rects.update},
      {L"再起動", rects.restart},
  }};
  for (const auto& [label, rect] : buttons) {
    DrawWidgetCard(dc, rect, kWidgetSurfaceAlt, CardRadius(rect));
    SetTextColor(dc, kWidgetText);
    DrawTextInRect(dc, label, rect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  }

  if (!nativeToast_.empty()) {
    SetTextColor(dc, kWidgetWarning);
    DrawTextInRect(dc, nativeToast_, rects.status, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
  } else {
    std::wstring radarTime;
    bool hasFrame = false;
    {
      std::lock_guard lock(radarFrameMutex_);
      radarTime = radarTimeText_;
      hasFrame = radarFrameBitmap_ != nullptr;
    }
    const std::wstring radarStatus = hasFrame
        ? L"レーダー更新: " + (radarTime.empty() ? L"--:--" : radarTime)
        : L"レーダーを準備中";
    const int statusHeight = static_cast<int>(rects.status.bottom - rects.status.top);
    RECT firstLine = rects.status;
    firstLine.bottom = rects.status.top + statusHeight / 2;
    RECT secondLine = rects.status;
    secondLine.top = firstLine.bottom;
    SetTextColor(dc, kWidgetMuted);
    DrawTextInRect(dc, radarStatus, firstLine,
                   DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
    if (!nativeAppVersion_.empty()) {
      SetTextColor(dc, kWidgetSubtle);
      DrawTextInRect(dc, L"バージョン " + nativeAppVersion_, secondLine,
                     DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
    }
  }
  SelectObject(dc, previousFont);
}

void Renderer::DrawMusicSection(HDC dc, const RECT& card) {
  const RECT content = CardContentRect(card);
  const RECT body = CardBodyRect(content);
  const MusicRowRects rects = MusicRowFromBody(body);
  const RECT& rowRect = rects.row;
  const int rowHeight = std::max(1, static_cast<int>(rowRect.bottom - rowRect.top));
  const int pad = std::max(6, rowHeight * 10 / 100);
  const int textLeft = rects.artwork.right + pad;
  const int textRight = rects.button.left - pad;
  const bool bothStationheadsOnFallback =
      IsStationheadFallbackUrl(nativeStationhead_.url, nativeStationhead_.fallbackUrl) &&
      IsStationheadFallbackUrl(nativeStationhead_.secondaryUrl, nativeStationhead_.fallbackUrl);
  const int64_t nowMs = UnixMillis();
  const NativePlaybackRender sharedPlayback =
      ResolveNativePlayback(bothStationheadsOnFallback ? 1 : 0, nowMs);
  const bool withProgress =
      sharedPlayback.hasTrack && sharedPlayback.track.durationMs > 0;

  const auto drawProgress = [&] {
    RECT barRect{textLeft, rowRect.top + rowHeight * 66 / 100,
                 textRight, rowRect.top + rowHeight * 72 / 100};
    DrawWidgetPill(dc, barRect, kWidgetTrack);
    const double ratio = std::clamp(
        static_cast<double>(sharedPlayback.progressMs) /
            static_cast<double>(sharedPlayback.track.durationMs),
        0.0, 1.0);
    RECT fillRect = barRect;
    fillRect.right = barRect.left +
        static_cast<int>((barRect.right - barRect.left) * ratio);
    if (fillRect.right > fillRect.left) {
      DrawWidgetPill(
          dc, fillRect, sharedPlayback.playing ? kWidgetGreen : kWidgetMuted);
    }

    SetTextColor(dc, kWidgetSubtle);
    RECT timeRect{textLeft, barRect.bottom + rowHeight * 3 / 100,
                  textRight, rowRect.bottom - rowHeight * 3 / 100};
    DrawTextInRect(dc, TrackTimeText(sharedPlayback.progressMs), timeRect,
                   DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    DrawTextInRect(dc, TrackTimeText(sharedPlayback.track.durationMs), timeRect,
                   DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
  };

  RECT clip{};
  GetClipBox(dc, &clip);
  const RECT progressRect = MusicProgressRectFromBody(body);
  const bool progressOnly = withProgress &&
      clip.left >= progressRect.left && clip.top >= progressRect.top &&
      clip.right <= progressRect.right && clip.bottom <= progressRect.bottom;
  if (progressOnly) {
    FillRect(dc, &progressRect, CachedSolidBrush(kWidgetSurface));
    HGDIOBJ previousFont = SelectObject(dc, TierFont(FontTier::Small));
    drawProgress();
    SelectObject(dc, previousFont);
    return;
  }

  DrawSectionCard(dc, card);
  HGDIOBJ previousFont = SelectObject(dc, TierFont(FontTier::Small));
  std::wstring dailyPlaySummary = L"直近1時間 --　本日 --　昨日 --";
  if (nativeStationhead_.dailyPlayStatsUpdatedAt > 0 &&
      !nativeStationhead_.dailyPlayCounts.empty()) {
    const auto& counts = nativeStationhead_.dailyPlayCounts;
    const int recentIncrease = RecentStationheadPlayIncrease(nativeStationheadPlayHistory_);
    const std::wstring recentText = recentIncrease >= 0
        ? std::to_wstring(recentIncrease)
        : L"--";
    dailyPlaySummary = L"直近1時間 " + recentText + L"　本日 " +
                       std::to_wstring(counts.back().value) + L"　昨日 ";
    dailyPlaySummary += counts.size() >= 2
        ? std::to_wstring(counts[counts.size() - 2].value)
        : L"--";
  }
  SetTextColor(dc, kWidgetSubtle);
  RECT dailyPlayHeader{content.left, content.top, content.right,
                       content.top + CardHeaderHeight(content)};
  DrawTextInRect(dc, dailyPlaySummary, dailyPlayHeader,
                 DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

  const int artworkRadius = std::max(6, rowHeight * 14 / 100);
  DrawWidgetCard(dc, rects.artwork, kWidgetSurfaceAlt, artworkRadius);
  if (sharedPlayback.hasTrack) {
    const int saved = SaveDC(dc);
    HRGN artworkClip = CreateRoundRectRgn(rects.artwork.left, rects.artwork.top,
                                          rects.artwork.right + 1, rects.artwork.bottom + 1,
                                          artworkRadius, artworkRadius);
    ExtSelectClipRgn(dc, artworkClip, RGN_AND);
    DeleteObject(artworkClip);
    DrawPremultipliedBitmap(dc,
                            NativeArtworkBitmap(sharedPlayback.track.artwork,
                                                rects.artwork.right - rects.artwork.left,
                                                rects.artwork.bottom - rects.artwork.top),
                            rects.artwork);
    RestoreDC(dc, saved);
  }

  const std::wstring title = sharedPlayback.hasTrack
      ? sharedPlayback.track.title
      : (bothStationheadsOnFallback ? L"Buddy46" : L"");
  const std::wstring artist = sharedPlayback.hasTrack ? sharedPlayback.track.artist : L"";
  const std::wstring detail = nativeStationhead_.loginRequired ? L"ログイン待ち"
      : nativeStationhead_.processFailed ? L"WebView再起動待ち"
      : nativeStationhead_.audioPlaying ? L"再生中"
      : nativeStationhead_.created ? L"接続中"
      : L"起動待ち";
  const std::wstring& rowDetail = sharedPlayback.available && !sharedPlayback.hasTrack
      ? L"次の曲を待機中"
      : detail;

  SetTextColor(dc, kWidgetText);
  SelectObject(dc, TierFont(FontTier::Medium));
  RECT titleRect{textLeft, rowRect.top + rowHeight * 6 / 100, textRight,
                 rowRect.top + rowHeight * 42 / 100};
  DrawTextInRect(dc, title.empty() ? L"--" : title, titleRect,
                 DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

  SetTextColor(dc, kWidgetMuted);
  SelectObject(dc, TierFont(FontTier::Small));
  RECT artistRect{textLeft, titleRect.bottom, textRight,
                  withProgress ? rowRect.top + rowHeight * 62 / 100 : rowRect.bottom - pad};
  DrawTextInRect(dc, artist.empty() ? rowDetail : artist, artistRect,
                 DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

  if (withProgress) drawProgress();

  DrawWidgetCard(dc, rects.button, kWidgetSuccessSurface, CardRadius(rects.button));
  SetTextColor(dc, kWidgetGreen);
  SelectObject(dc, TierFont(FontTier::Small));
  DrawTextInRect(dc, nativeStationhead_.primaryAudioSelected ? L"音声A" : L"音声B",
                 rects.button, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

  DrawWidgetCard(dc, rects.muteButton, kWidgetDangerSurface,
                 CardRadius(rects.muteButton));
  SetTextColor(dc, kWidgetDanger);
  DrawTextInRect(dc, L"MUTE", rects.muteButton,
                 DT_CENTER | DT_SINGLELINE | DT_VCENTER);

  const NativeMinuteFactsProjection minuteFacts = NativeMinuteFactsSnapshot();
  std::wstring minuteFactsStatus = L"データ待ち";
  if (minuteFacts.available) {
    minuteFactsStatus = minuteFacts.isBroadcasting
        ? (minuteFacts.isPaused ? L"一時停止中" : L"配信中")
        : L"オフライン";
    minuteFactsStatus += L"・" + std::to_wstring(minuteFacts.listenerCount) + L"人";
    if (minuteFacts.stale) minuteFactsStatus += L"(遅延)";
  }
  SetTextColor(dc, kWidgetSubtle);
  DrawTextInRect(dc, minuteFactsStatus, rects.status,
                 DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
  SelectObject(dc, previousFont);
}

// Stationhead play history is recorded by App; no history bar is rendered here.
