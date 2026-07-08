# HomePanel dashboard modernization

## CO2

- Keep the current value prominent.
- Render a time-aware 60-minute history chart.
- Use a stable 400-2000 ppm vertical scale.
- Show subtle 1000 ppm and 1500 ppm guides and state zones.
- Preserve the last valid history during sensor interruptions and break the line across real gaps.

## Weather

- WeatherNews is the only forecast source.
- Do not calculate or display a multi-provider average.
- Do not display provider or city labels in the forecast panel.
- Display only time, temperature, precipitation probability and precipitation amount.

## Octopus Energy

- Treat SwitchBot and Octopus credentials as independent Cloudflare secrets.
- Surface the stored `current_state.error` as a short actionable message.
- Preserve the last valid usage values while stale.
- Distinguish missing credentials, authentication, account and readings failures.

## Spotify / Stationhead

- Spotify Web API is authoritative for track metadata.
- Display artwork, title, artist, elapsed time, duration and progress.
- Poll the playback API every ten minutes.
- Infer the active station item and elapsed time locally between API reads using the queue status timestamps.
- Stationhead WebView2 remains responsible for playback.

### Stationhead WebView2 viewport

- The playback WebView2 window must use an extremely tall vertical viewport rather than enlarging the dashboard card.
- Target viewport height: 12000 CSS/device-independent pixels unless WebView2 or the host display imposes a lower safe limit.
- Keep the viewport width at the current playback width.
- Move or clip the oversized WebView outside the visible dashboard instead of allowing it to cover the UI.
- Reapply the oversized bounds after controller creation, navigation recovery and Stationhead reconnect.
- The purpose is to keep vertically distant Stationhead controls/content instantiated without showing the full browser surface.

## Native dashboard migration

- The dashboard WebView has been removed; dashboard surfaces are native-owned child windows.
- The clock/date surface is native-owned and drawn in a sibling child window.
- News headline and hourly weather summary surfaces are native-owned and read from the parsed dashboard snapshot.
- Air current metrics and the 24-hour air history chart are native-owned.
- Octopus Energy summary and history chart are native-owned and read from the parsed dashboard snapshot.
- Stationhead panel shell, metadata, artwork, progress, and audio toggles are native-owned; the playback API queue projection is parsed and advanced natively on a one-second panel timer.
- Controls are native-owned and dispatch the existing update/restart actions through the renderer action queue.
- Toast feedback is native-owned and drawn in the controls panel.
- The browser-side second pulse has been removed; no browser panel performs local interpolation.
- All panel scripts have been removed from the dashboard page; do not reintroduce browser-side dashboard DOM updates for native-owned surfaces.
- Runtime-installed dashboard browser assets have been removed. The native installer only deploys the radar base images that native radar composition still reads from disk.
- Native dashboard panel updates are diffed so routine state changes invalidate only the affected child windows.
- Native panel timers are centralized through the app tick; individual clock/playback child-window timers are not used.
- Native panel back buffers and Stationhead artwork bitmaps are bounded/reused to reduce paint-time allocations.

## Radar

- Remove previous, play/pause, next and seek controls.
- Cloudflare continues to provide frame and tile metadata; the sync client localizes tiles into the radar cache.
- The current frame is composed natively on a worker thread from the bundled base layers and cached tiles, then painted in a native child window.
- Native radar composition does not fetch remote tile URLs; missing or uncached tiles are skipped so network work stays in the sync client.
- The last composed radar frame is cached on disk and reused when the radar metadata/base-layer signature has not changed.
- Failed tile decodes are suppressed briefly to avoid repeated decode attempts during cache misses.
- Keep the last successfully rendered frame when a tile fails.
- Recomposition happens only when radar metadata updates; no browser canvas or animation loop remains.
