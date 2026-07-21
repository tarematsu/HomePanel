namespace {
std::wstring DashboardStatusTimeText(int64_t epochMs) {
  if (epochMs <= 0) return L"--:--";
  const std::time_t seconds = static_cast<std::time_t>(epochMs / 1000);
  std::tm local{};
  if (localtime_s(&local, &seconds) != 0) return L"--:--";
  wchar_t text[16]{};
  if (wcsftime(text, std::size(text), L"%H:%M", &local) == 0) return L"--:--";
  return text;
}
}  // namespace

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
  const NativePlaybackRender sharedPlayback = bothStationheadsOnFallback
      ? NativePlaybackRender{}
      : ResolveNativePlayback(0, nowMs);
  const bool withProgress = !bothStationheadsOnFallback &&
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

  struct DailyPlaySummaryTextCache {
    const Renderer* owner = nullptr;
    const void* pointsIdentity = nullptr;
    int64_t statsUpdatedAt = -1;
    int64_t dayOrdinal = std::numeric_limits<int64_t>::min();
    std::wstring text;
  };
  static DailyPlaySummaryTextCache dailyCache;
  // Stationhead daily counts are keyed by UTC date. UTC midnight is 09:00 JST,
  // so the displayed "today" changes at the requested local boundary without
  // translating the source dates into another calendar.
  const int64_t dayOrdinal = StationheadUtcDayOrdinal(nowMs);
  const void* pointsIdentity = nativeStationhead_.dailyPlayCounts.identity();
  if (dailyCache.owner != this || dailyCache.pointsIdentity != pointsIdentity ||
      dailyCache.statsUpdatedAt != nativeStationhead_.dailyPlayStatsUpdatedAt ||
      dailyCache.dayOrdinal != dayOrdinal) {
    StationheadPlayPeriodSummary playSummary;
    if (nativeStationhead_.dailyPlayStatsUpdatedAt > 0 &&
        !nativeStationhead_.dailyPlayCounts.empty()) {
      playSummary = SummarizeStationheadDailyPlays(
          nativeStationhead_.dailyPlayCounts, nowMs);
    }
    dailyCache.owner = this;
    dailyCache.pointsIdentity = pointsIdentity;
    dailyCache.statsUpdatedAt = nativeStationhead_.dailyPlayStatsUpdatedAt;
    dailyCache.dayOrdinal = dayOrdinal;
    dailyCache.text =
        L"　本日 " + playValueText(playSummary.today) +
        L"　昨日 " + playValueText(playSummary.yesterday) +
        L"　今週 " + playValueText(playSummary.thisWeek) +
        L"　先週 " + playValueText(playSummary.lastWeek);
  }
  const std::wstring dailyPlaySummary =
      L"直近1時間 " + recentText + dailyCache.text;
  SetTextColor(dc, kWidgetSubtle);
  RECT dailyPlayHeader{content.left, content.top, content.right,
                       content.top + CardHeaderHeight(content)};
  DrawTextInRect(dc, dailyPlaySummary, dailyPlayHeader,
                 DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

  const int artworkRadius = std::max(6, rowHeight * 14 / 100);
  DrawWidgetCard(dc, rects.artwork, kWidgetSurfaceAlt, artworkRadius);
  if (!bothStationheadsOnFallback && sharedPlayback.hasTrack) {
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

  const std::wstring title = bothStationheadsOnFallback
      ? L"fallback中"
      : (sharedPlayback.hasTrack ? sharedPlayback.track.title : L"");
  const std::wstring artist = sharedPlayback.hasTrack ? sharedPlayback.track.artist : L"";
  const std::wstring detail = nativeStationhead_.loginRequired ? L"ログイン待ち"
      : nativeStationhead_.processFailed ? L"WebView再起動待ち"
      : nativeStationhead_.audioPlaying ? L"再生中"
      : nativeStationhead_.created ? L"接続中"
      : L"起動待ち";
  const std::wstring rowDetail =
      sharedPlayback.available && !sharedPlayback.hasTrack ? L"次の曲を待機中" : detail;

  SetTextColor(dc, kWidgetText);
  SelectObject(dc, TierFont(FontTier::Medium));
  RECT titleRect{textLeft, rowRect.top + rowHeight * 6 / 100, textRight,
                 rowRect.top + rowHeight * 42 / 100};
  DrawTextInRect(dc, title.empty() ? L"--" : title, titleRect,
                 DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

  if (!bothStationheadsOnFallback) {
    SetTextColor(dc, kWidgetMuted);
    SelectObject(dc, TierFont(FontTier::Small));
    RECT artistRect{textLeft, titleRect.bottom, textRight,
                    withProgress ? rowRect.top + rowHeight * 62 / 100 : rowRect.bottom - pad};
    DrawTextInRect(dc, artist.empty() ? rowDetail : artist, artistRect,
                   DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
  }

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

  const NativeMinuteFactsProjection stationheadStatus = NativeMinuteFactsSnapshot();
  std::wstring stateText = L"状態取得待ち";
  if (stationheadStatus.available) {
    if (stationheadStatus.isPaused) {
      stateText = L"一時停止";
    } else if (stationheadStatus.isBroadcasting) {
      stateText = L"配信中 " + std::to_wstring(stationheadStatus.listenerCount);
    } else {
      stateText = L"停止中";
    }
  }
  const std::wstring timeText =
      L"生成" + DashboardStatusTimeText(stationheadStatus.minuteAt) +
      L" 取得" + DashboardStatusTimeText(stationheadStatus.fetchedAt);
  const LONG statusMiddle = rects.status.top +
      std::max<LONG>(1, (rects.status.bottom - rects.status.top) / 2);
  RECT stateRect = rects.status;
  stateRect.bottom = statusMiddle;
  RECT timeRect = rects.status;
  timeRect.top = statusMiddle;
  SetTextColor(dc, stationheadStatus.available ? kWidgetMuted : kWidgetSubtle);
  DrawTextInRect(dc, stateText, stateRect,
                 DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
  DrawTextInRect(dc, timeText, timeRect,
                 DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

  SelectObject(dc, previousFont);
}

// Stationhead play history is recorded by App; no history bar is rendered here.
