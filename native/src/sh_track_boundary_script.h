#pragma once
#include "common.h"

namespace hp {

inline std::wstring StationheadTrackBoundaryScript(const wchar_t* messagePrefix) {
  static constexpr wchar_t kTemplate[] = LR"JS(;(() => {
  const host = String(location.hostname || '').toLowerCase();
  if (host !== 'stationhead.com' && !host.endsWith('.stationhead.com')) return;
  if (window.__homepanelStationheadTrackBoundaryBridge) return;
  window.__homepanelStationheadTrackBoundaryBridge = true;

  let pageActive = true;
  let playedMedia = new WeakSet();
  window.addEventListener('pagehide', () => {
    pageActive = false;
    playedMedia = new WeakSet();
  });
  window.addEventListener('pageshow', () => { pageActive = true; });

  const rememberPlayedMedia = event => {
    if (pageActive && event.target instanceof HTMLMediaElement) {
      playedMedia.add(event.target);
    }
  };
  document.addEventListener('play', rememberPlayedMedia, true);
  document.addEventListener('playing', rememberPlayedMedia, true);
  document.addEventListener('ended', event => {
    const media = event.target;
    if (!(media instanceof HTMLMediaElement) || !playedMedia.has(media)) return;
    if (!pageActive) {
      playedMedia.delete(media);
      return;
    }

    // Stationhead may reuse the same element for the next track before the old
    // ended event is delivered. Keep tracking it and do not interrupt the new track.
    const sameMediaRestarted = !media.paused && !media.ended &&
      media.readyState >= HTMLMediaElement.HAVE_CURRENT_DATA;
    if (sameMediaRestarted) return;

    // Ignore stale/auxiliary media ending while another Stationhead media
    // element is already carrying the next track. Refresh at an actual gap.
    const anotherMediaIsPlaying = Array.from(document.querySelectorAll('audio,video')).some(
      candidate => candidate !== media && !candidate.paused && !candidate.ended &&
        candidate.readyState >= HTMLMediaElement.HAVE_CURRENT_DATA);
    playedMedia.delete(media);
    if (anotherMediaIsPlaying) return;

    try {
      window.chrome?.webview?.postMessage('{{PREFIX}}-track-ended');
    } catch (_) {}
  }, true);
})()
)JS";
  // This script is appended after the existing autoplay IIFE. The leading
  // semicolon prevents automatic semicolon insertion from treating it as a
  // call on the previous IIFE's return value.
  static_assert(kTemplate[0] == L';');
  std::wstring script = kTemplate;
  constexpr std::wstring_view placeholder = L"{{PREFIX}}";
  const std::wstring replacement = messagePrefix ? messagePrefix : L"stationhead";
  for (size_t at = script.find(placeholder); at != std::wstring::npos;
       at = script.find(placeholder, at + replacement.size())) {
    script.replace(at, placeholder.size(), replacement);
  }
  return script;
}

}  // namespace hp
