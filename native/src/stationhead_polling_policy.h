#pragma once

// Keep the shared helpers available, but replace the two authenticated API
// helpers with policy-specific implementations for the native application.
#define StationheadApiPlayStatsScript StationheadApiPlayStatsScriptUnthrottled
#define StationheadAuthProbeScript StationheadAuthProbeScriptNetwork
#include "sh_shared.h"
#undef StationheadAuthProbeScript
#undef StationheadApiPlayStatsScript

namespace hp {

// Window A may ask for stats more frequently while recovering authentication,
// but a successful authenticated request is followed by a ten-minute quiet
// period. Failed/no-header attempts keep the existing short retry behavior.
inline std::wstring StationheadApiPlayStatsScript(int channelId) {
  std::wostringstream script;
  script << LR"JS(
(() => {
  const post = message => {
    try { window.chrome?.webview?.postMessage(message); } catch (_) {}
  };
  const headers = window.__homepanelStationheadAuthHeaders;
  if (!headers?.authorization) {
    post({ type: 'stationhead-play-stats-error', error: 'no-auth-header' });
    return false;
  }
  const lastSuccessAt = Number(window.__homepanelStationheadPlayStatsSuccessAt || 0);
  if (lastSuccessAt > 0 && Date.now() - lastSuccessAt < 10 * 60 * 1000) {
    return false;
  }
  const url = 'https://production1.stationhead.com/me/channel/)JS"
         << channelId << LR"JS(/streakStats';
  fetch(url, {
    method: 'GET',
    credentials: 'include',
    cache: 'no-store',
    headers: Object.assign({ accept: 'application/json' }, headers),
  }).then(async response => {
    if (response.status === 401 || response.status === 403) {
      window.__homepanelStationheadRejectedAuthorization = headers.authorization;
      window.__homepanelStationheadAuthHeaders = null;
      post({ type: 'stationhead-play-stats-auth-failed', status: response.status });
      return null;
    }
    if (!response.ok) throw new Error('http-' + response.status);
    return response.json();
  }).then(data => {
    if (data) {
      window.__homepanelStationheadPlayStatsSuccessAt = Date.now();
      post({ type: 'stationhead-play-stats', data, source: 'authenticated-api' });
    }
  }).catch(error => {
    post({ type: 'stationhead-play-stats-error', error: String(error?.message || error) });
  });
  return true;
})()
)JS";
  return script.str();
}

// Window B must not make an extra logged-in API request. Its periodic probe now
// inspects only the authorization header already observed from the page's own
// traffic and immediately reports that local state to the native handler.
inline std::wstring StationheadAuthProbeScript(int channelId) {
  (void)channelId;
  static constexpr wchar_t kScript[] = LR"JS(
(() => {
  const post = message => {
    try { window.chrome?.webview?.postMessage(message); } catch (_) {}
  };
  const authorized = Boolean(window.__homepanelStationheadAuthHeaders?.authorization);
  post({ type: 'stationhead-auth-probe', state: authorized ? 'ok' : 'no-auth-header' });
  return authorized;
})()
)JS";
  return kScript;
}

}  // namespace hp
