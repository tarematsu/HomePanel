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
  const auto playValueText = [](int64_t value) {
    return value >= 0 ? std::to_wstring(value) : std::wstring(L"--");
  };
  const int recentIncrease = RecentStationheadPlayIncrease(nativeStationheadPlayHistory_);
  const std::wstring recentText = playValueText(recentIncrease);
  StationheadPlayPeriodSummary playSummary;
  if (nativeStationhead_.dailyPlayStatsUpdatedAt > 0 &&
      !nativeStationhead_.dailyPlayCounts.empty()) {
    playSummary = SummarizeStationheadDailyPlays(
        nativeStationhead_.dailyPlayCounts, nowMs);
  }
  const std::wstring dailyPlaySummary =
      L"直近1時間 " + recentText +
      L"　本日 " + playValueText(playSummary.today) +
      L"　昨日 " + playValueText(playSummary.yesterday) +
      L"　今週 " + playValueText(playSummary.thisWeek) +
      L"　先週 " + playValueText(playSummary.lastWeek);
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
  std::wstring rowDetail = detail;
  if (sharedPlayback.setupRequired) {
    rowDetail = L"再生設定待ち";
  } else if (sharedPlayback.ended) {
    rowDetail = sharedPlayback.stale ? L"キュー終了・情報遅延" : L"キュー終了";
  } else if (sharedPlayback.stale) {
    rowDetail = L"再生情報を更新待ち";
  } else if (sharedPlayback.available && !sharedPlayback.hasTrack) {
    rowDetail = L"次の曲を待機中";
  }

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
  SelectObject(dc, previousFont);
}

// Stationhead play history is recorded by App; no history bar is rendered here.
