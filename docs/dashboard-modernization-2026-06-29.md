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

- Migrate static dashboard panels incrementally before replacing the full dashboard WebView.
- The clock/date surface is native-owned and drawn in a sibling child window above the dashboard WebView.
- News headline and hourly weather summary surfaces are native-owned and read from the parsed dashboard snapshot.
- Air current metrics and the 24-hour air history chart are native-owned.
- Octopus Energy summary and history chart are native-owned and read from the parsed dashboard snapshot.
- Controls are native-owned and dispatch the existing update/restart actions through the renderer action queue.
- Keep the browser-side second pulse only for panels that still need local interpolation, such as Spotify progress.
- Do not reintroduce browser-side clock DOM updates once a static surface has moved to native rendering.

## Radar

- Remove previous, play/pause, next and seek controls.
- Start animation automatically and loop continuously.
- Cloudflare continues to provide frame and tile metadata.
- The client composes the current frame and preloads the next one.
- Keep the last successfully rendered frame when a tile fails.
- Stop animation and decoding while the monitor is off, then resume with fresh frame metadata.
