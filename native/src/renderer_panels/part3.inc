}

void Renderer::DrawClockSection(HDC dc, const RECT& card) {
  DrawSectionCard(dc, card);
  const RECT content = CardContentRect(card);
  SYSTEMTIME now{};
  GetLocalTime(&now);

  RECT dateRect{content.left, content.top, content.right, content.top + SpanY(content, 320)};
  HGDIOBJ previousFont = SelectObject(dc, TierFont(FontTier::Small));
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
  SetTextColor(dc, kWidgetSubtle);
  RECT header{content.left, content.top, content.right, content.top + CardHeaderHeight(content)};
  DrawTextInRect(dc, L"室内環境", header, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  const RECT body = CardBodyRect(content);

  const bool connected = nativeSensors_.co2Connected;
  const COLORREF co2Color = !connected ? kWidgetMuted
      : nativeSensors_.co2 >= 1500 ? kWidgetDanger
      : nativeSensors_.co2 >= 1000 ? kWidgetWarning
      : kWidgetGreen;
  struct AirStat {
    std::wstring label;
    std::wstring value;
    COLORREF color;
  };
  const std::array<AirStat, 3> stats{{
      {L"CO2", connected ? std::to_wstring(nativeSensors_.co2) + L" ppm" : L"--- ppm", co2Color},
      {L"温度", connected ? Fixed(nativeSensors_.temperatureCorrected, 1) + L"℃" : L"--.-℃",
       kWidgetText},
      {L"湿度", connected ? Fixed(nativeSensors_.humidityCorrected, 0) + L"%" : L"--%",
       kWidgetText},
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

  const int legendTop = body.top + statsHeight + SpanY(body, 60);
  const int legendHeight = std::clamp(SpanY(body, 110), 14, 26);
  SelectObject(dc, TierFont(FontTier::Small));
  RECT legend{body.left, legendTop, body.right, legendTop + legendHeight};
  SetTextColor(dc, kWidgetSubtle);
  DrawTextInRect(dc, L"24時間の推移", legend, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  const std::array<std::pair<const wchar_t*, COLORREF>, 3> legendKeys{{
      {L"CO2", kWidgetGreen}, {L"温度", kWidgetOrange}, {L"湿度", kWidgetBlueMuted},
  }};
  const int keyWidth = std::max(34, SpanX(body, 120));
  int keyRight = body.right;
  for (int i = 2; i >= 0; --i) {
    RECT keyRect{keyRight - keyWidth, legend.top, keyRight, legend.bottom};
    SetTextColor(dc, legendKeys[static_cast<size_t>(i)].second);
    DrawTextInRect(dc, legendKeys[static_cast<size_t>(i)].first, keyRect,
                   DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    keyRight -= keyWidth;
  }

  const int axisHeight = std::clamp(SpanY(body, 100), 12, 22);
  RECT plot{body.left, legend.bottom + SpanY(body, 30), body.right,
            body.bottom - axisHeight - SpanY(body, 15)};
  HPEN gridPen = CreatePen(PS_SOLID, 1, kWidgetTrack);
  HGDIOBJ previousPen = SelectObject(dc, gridPen);
  for (int i = 0; i < 4; ++i) {
    const int y = plot.top + (plot.bottom - plot.top) * i / 3;
    MoveToEx(dc, plot.left, y, nullptr);
    LineTo(dc, plot.right, y);
  }
  SelectObject(dc, previousPen);
  DeleteObject(gridPen);

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

  SetTextColor(dc, kWidgetSubtle);
  RECT axis{body.left, body.bottom - axisHeight, body.right, body.bottom};
  DrawTextInRect(dc, L"24時間前", axis, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  DrawTextInRect(dc, L"現在", axis, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
  SelectObject(dc, previousFont);
}

void Renderer::DrawWeatherSection(HDC dc, const RECT& card) {
  DrawSectionCard(dc, card);
  const RECT content = CardContentRect(card);
  const auto& hours = nativeDashboard_.weatherHours;
  const size_t count = std::min<size_t>(5, hours.size());
  double maxPop = 0;
  for (size_t i = 0; i < count; ++i) {
    if (std::isfinite(hours[i].precipitationProbability)) {
      maxPop = std::max(maxPop, hours[i].precipitationProbability);
    }
  }

  HGDIOBJ previousFont = SelectObject(dc, TierFont(FontTier::Small));
  RECT header{content.left, content.top, content.right, content.top + CardHeaderHeight(content)};
  SetTextColor(dc, kWidgetSubtle);
  DrawTextInRect(dc, L"天気予報", header, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  if (nativeDashboard_.weatherStatus.state == PanelDataState::Ok) {
    SetTextColor(dc, maxPop >= 70 ? kWidgetBlue : maxPop >= 40 ? kWidgetBlueMuted : kWidgetMuted);
    DrawTextInRect(dc,
                   L"降水確率 " + std::to_wstring(static_cast<int>(std::round(maxPop))) + L"%",
                   header, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
  } else {
    DrawHeaderStatus(dc, header, nativeDashboard_.weatherStatus);
  }

  const RECT body = CardBodyRect(content);
  const int bodyHeight = std::max(1L, body.bottom - body.top);
  const int columnGap = std::max(4, SpanX(body, 25));
  constexpr int kSlotCount = 5;
  const int columnWidth = std::max(
      1, (static_cast<int>(body.right - body.left) - columnGap * (kSlotCount - 1)) / kSlotCount);
  for (int i = 0; i < kSlotCount; ++i) {
    const int left = body.left + i * (columnWidth + columnGap);
    const RECT column{left, body.top, left + columnWidth, body.bottom};
    const bool hasData = i < static_cast<int>(count);
    const bool hasIcon = hasData && !hours[static_cast<size_t>(i)].icon.empty();

    SetTextColor(dc, kWidgetMuted);
    SelectObject(dc, TierFont(FontTier::Small));
    RECT hourRect{column.left, column.top, column.right, column.top + bodyHeight * 18 / 100};
    const std::wstring hour = hasData
        ? std::to_wstring(hours[static_cast<size_t>(i)].hour) + L"時" : L"--";
    DrawTextInRect(dc, hour, hourRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    int iconBottom = column.top + bodyHeight * 22 / 100;
    if (hasIcon) {
      const int iconWidth = std::min(columnWidth, bodyHeight * 34 / 100 * 90 / 48);
      const int iconHeight = std::max(12, iconWidth * 48 / 90);
      const int iconTop = column.top + bodyHeight * 22 / 100;
      RECT iconRect{column.left + (columnWidth - iconWidth) / 2, iconTop,
                    column.left + (columnWidth + iconWidth) / 2, iconTop + iconHeight};
      DrawPremultipliedBitmap(dc,
          NativeWeatherIconBitmap(hours[static_cast<size_t>(i)].icon,
                                  IsWeatherNightHour(hours[static_cast<size_t>(i)].hour),
                                  iconWidth, iconHeight),
          iconRect);
      // Narrow columns can make the icon shorter than its intended 34% slot.
      // Use the actual bitmap end so the temperature does not float below it.
      iconBottom = iconRect.bottom;
    }

    SetTextColor(dc, kWidgetText);
    SelectObject(dc, TierFont(FontTier::Medium));
    const int temperatureTop = hasIcon
        ? iconBottom
        : column.top + bodyHeight * 56 / 100;
    const int temperatureHeight = std::clamp(bodyHeight * 12 / 100, 20, 32);
    RECT tempRect{column.left, temperatureTop, column.right,
                  std::min(static_cast<int>(column.bottom), temperatureTop + temperatureHeight)};
    const std::wstring temp = hasData
        ? NumberOrDash(hours[static_cast<size_t>(i)].temperature, 0) + L"℃" : L"--";
    DrawTextInRect(dc, temp, tempRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    const double rainMm = hasData ? hours[static_cast<size_t>(i)].rainMm
                                  : std::numeric_limits<double>::quiet_NaN();
    SetTextColor(dc, std::isfinite(rainMm) && rainMm > 0 ? kWidgetBlueMuted : kWidgetSubtle);
    SelectObject(dc, TierFont(FontTier::Small));
    RECT rainRect{column.left, column.top + bodyHeight * 80 / 100, column.right, column.bottom};
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
  DrawSectionCard(dc, card);
  const RECT content = CardContentRect(card);
  HGDIOBJ previousFont = SelectObject(dc, TierFont(FontTier::Small));
  SetTextColor(dc, kWidgetSubtle);
  RECT header{content.left, content.top, content.right, content.top + CardHeaderHeight(content)};
  DrawTextInRect(dc, L"ミュージック", header, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
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
  RECT dailyPlayHeader = header;
  dailyPlayHeader.left += SpanX(header, 120);
  DrawTextInRect(dc, dailyPlaySummary, dailyPlayHeader,
                 DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
  RECT body = CardBodyRect(content);
  const int64_t nowMs = UnixMillis();
  const NativePlaybackRender playbackA = ResolveNativePlayback(0, nowMs);
  const NativePlaybackRender playbackB = ResolveNativePlayback(1, nowMs);
  const bool bothStationheadsOnFallback =
      IsStationheadFallbackUrl(nativeStationhead_.url, nativeStationhead_.fallbackUrl) &&
      IsStationheadFallbackUrl(nativeStationhead_.secondaryUrl, nativeStationhead_.fallbackUrl);
  const NativePlaybackRender& sharedPlayback =
      bothStationheadsOnFallback ? playbackB : playbackA;

  const bool primaryAudioSelected = nativeStationhead_.primaryAudioSelected;
  const auto drawRow = [&](const NativePlaybackRender& playback,
                           const std::wstring& fallbackTitle, const std::wstring& detail) {
    const MusicRowRects rects = MusicRowFromBody(body);
    const RECT& rowRect = rects.row;
    const int rowHeight = static_cast<int>(rowRect.bottom - rowRect.top);
    const int pad = std::max(6, rowHeight * 10 / 100);

    const int artworkRadius = std::max(6, rowHeight * 14 / 100);
    DrawWidgetCard(dc, rects.artwork, kWidgetSurfaceAlt, artworkRadius);
    if (playback.hasTrack) {
      const int saved = SaveDC(dc);
      HRGN artworkClip = CreateRoundRectRgn(rects.artwork.left, rects.artwork.top,
                                            rects.artwork.right + 1, rects.artwork.bottom + 1,
                                            artworkRadius, artworkRadius);
      ExtSelectClipRgn(dc, artworkClip, RGN_AND);
      DeleteObject(artworkClip);
      DrawPremultipliedBitmap(dc,
                              NativeArtworkBitmap(playback.track.artwork,
                                                  rects.artwork.right - rects.artwork.left,
                                                  rects.artwork.bottom - rects.artwork.top),
                              rects.artwork);
      RestoreDC(dc, saved);
    }

    const std::wstring title = playback.hasTrack ? playback.track.title : fallbackTitle;
    const std::wstring artist = playback.hasTrack ? playback.track.artist : L"";
    const bool withProgress = playback.hasTrack && playback.track.durationMs > 0;
    const int textLeft = rects.artwork.right + pad;
    const int textRight = rects.button.left - pad;

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
    DrawTextInRect(dc, artist.empty() ? detail : artist, artistRect,
                   DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

    if (withProgress) {
      RECT barRect{textLeft, rowRect.top + rowHeight * 66 / 100,
                   textRight, rowRect.top + rowHeight * 72 / 100};
      DrawWidgetPill(dc, barRect, kWidgetTrack);
      const double ratio = std::clamp(
          static_cast<double>(playback.progressMs) / static_cast<double>(playback.track.durationMs),
          0.0, 1.0);
      RECT fillRect = barRect;
      fillRect.right = barRect.left +
          static_cast<int>((barRect.right - barRect.left) * ratio);
      if (fillRect.right > fillRect.left) {
        DrawWidgetPill(dc, fillRect, playback.playing ? kWidgetGreen : kWidgetMuted);
      }

      SetTextColor(dc, kWidgetSubtle);
      RECT timeRect{textLeft, barRect.bottom + rowHeight * 3 / 100,
                    textRight, rowRect.bottom - rowHeight * 3 / 100};
      DrawTextInRect(dc, TrackTimeText(playback.progressMs), timeRect,
                     DT_LEFT | DT_SINGLELINE | DT_VCENTER);
      DrawTextInRect(dc, TrackTimeText(playback.track.durationMs), timeRect,
                     DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }

    DrawWidgetCard(dc, rects.button, kWidgetSuccessSurface, CardRadius(rects.button));
    SetTextColor(dc, kWidgetGreen);
    SelectObject(dc, TierFont(FontTier::Small));
    DrawTextInRect(dc, primaryAudioSelected ? L"A 音声ON" : L"B 音声ON", rects.button,
                   DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    DrawWidgetCard(dc, rects.muteButton, kWidgetDangerSurface,
                   CardRadius(rects.muteButton));
    SetTextColor(dc, kWidgetDanger);
    DrawTextInRect(dc, L"MUTE", rects.muteButton,
                   DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  };

  const std::wstring detail = nativeStationhead_.loginRequired ? L"ログイン待ち"
      : nativeStationhead_.processFailed ? L"WebView再起動待ち"
      : nativeStationhead_.audioPlaying ? L"再生中"
      : nativeStationhead_.created ? L"接続中"
      : L"起動待ち";
  drawRow(sharedPlayback, bothStationheadsOnFallback ? L"Buddy46" : L"",
          sharedPlayback.available && !sharedPlayback.hasTrack ? L"次の曲を待機中" : detail);
  SelectObject(dc, previousFont);
}

// Stationhead play history is recorded by App; no history bar is rendered here.
