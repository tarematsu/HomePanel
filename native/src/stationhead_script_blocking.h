#pragma once

namespace hp {

// Match only independently named Stationhead presentation chunks. Core player,
// authentication, queue and realtime code deliberately remain outside this
// list because those bundles are required for unattended playback.
inline constexpr bool StationheadExtraNonPlaybackScriptUrl(
    std::wstring_view uriLower) {
  const size_t schemeEnd = uriLower.find(L"://");
  if (schemeEnd == std::wstring_view::npos) return false;
  const size_t hostAt = schemeEnd + 3;
  const size_t pathAt = uriLower.find_first_of(L"/?#", hostAt);
  const std::wstring_view host = uriLower.substr(
      hostAt, pathAt == std::wstring_view::npos ? std::wstring_view::npos
                                                : pathAt - hostAt);
  if (host != L"stationhead.com" && !host.ends_with(L".stationhead.com")) {
    return false;
  }
  if (pathAt == std::wstring_view::npos || uriLower[pathAt] != L'/') return false;
  const size_t pathEnd = uriLower.find_first_of(L"?#", pathAt);
  const std::wstring_view path = uriLower.substr(
      pathAt, pathEnd == std::wstring_view::npos ? std::wstring_view::npos
                                                 : pathEnd - pathAt);
  if (!path.ends_with(L".js") && !path.ends_with(L".mjs")) return false;

  constexpr std::wstring_view kNeedles[] = {
      L"notification", L"notifications", L"inbox", L"message-center",
      L"messages-panel", L"share-sheet", L"sharing", L"invite", L"referral",
      L"followers", L"following", L"follow-modal", L"discover", L"discovery",
      L"social-feed", L"activity-feed", L"leaderboard", L"audience-modal",
      L"listener-modal", L"listeners-modal",
  };
  for (const std::wstring_view needle : kNeedles) {
    if (path.find(needle) != std::wstring_view::npos) return true;
  }
  return false;
}

static_assert(StationheadExtraNonPlaybackScriptUrl(
    L"https://www.stationhead.com/_next/static/chunks/notifications-panel.123.js"));
static_assert(StationheadExtraNonPlaybackScriptUrl(
    L"https://stationhead.com/assets/share-sheet-a1b2.mjs?build=2"));
static_assert(!StationheadExtraNonPlaybackScriptUrl(
    L"https://www.stationhead.com/assets/player-runtime-a1b2.js"));
static_assert(!StationheadExtraNonPlaybackScriptUrl(
    L"https://www.stationhead.com/assets/auth-session-a1b2.js"));
static_assert(!StationheadExtraNonPlaybackScriptUrl(
    L"https://www.stationhead.com/assets/queue-realtime-a1b2.js"));
static_assert(!StationheadExtraNonPlaybackScriptUrl(
    L"https://cdn.example.com/assets/notifications-panel.js"));

inline void ApplyStationheadExtraScriptBlocking(
    ICoreWebView2Environment* environment,
    ICoreWebView2* webview) {
  if (!environment || !webview) return;
  webview->AddWebResourceRequestedFilter(
      L"https://stationhead.com/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_SCRIPT);
  webview->AddWebResourceRequestedFilter(
      L"https://*.stationhead.com/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_SCRIPT);

  ComPtr<ICoreWebView2Environment> env = environment;
  EventRegistrationToken ignoredToken{};
  webview->add_WebResourceRequested(
      Callback<ICoreWebView2WebResourceRequestedEventHandler>(
          [env](ICoreWebView2*,
                ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
            if (!args) return S_OK;
            COREWEBVIEW2_WEB_RESOURCE_CONTEXT context =
                COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL;
            if (FAILED(args->get_ResourceContext(&context)) ||
                context != COREWEBVIEW2_WEB_RESOURCE_CONTEXT_SCRIPT) {
              return S_OK;
            }
            ComPtr<ICoreWebView2WebResourceRequest> request;
            if (FAILED(args->get_Request(&request)) || !request) return S_OK;
            LPWSTR uriRaw = nullptr;
            if (FAILED(request->get_Uri(&uriRaw)) || !uriRaw) return S_OK;
            const std::wstring uriLower = StationheadLowerAscii(uriRaw);
            CoTaskMemFree(uriRaw);
            if (!StationheadExtraNonPlaybackScriptUrl(uriLower)) return S_OK;

            ComPtr<ICoreWebView2WebResourceResponse> response;
            if (SUCCEEDED(env->CreateWebResourceResponse(
                    nullptr, 403, L"Blocked non-playback script", 
                    L"Content-Type: application/javascript; charset=utf-8",
                    &response))) {
              args->put_Response(response.Get());
            }
            return S_OK;
          }).Get(),
      &ignoredToken);
}

}  // namespace hp
