#pragma once

namespace hp {

inline constexpr bool StationheadHostUrl(std::wstring_view uriLower) {
  const size_t schemeEnd = uriLower.find(L"://");
  if (schemeEnd == std::wstring_view::npos) return false;
  const size_t hostAt = schemeEnd + 3;
  const size_t hostEnd = uriLower.find_first_of(L"/?#", hostAt);
  const std::wstring_view host = uriLower.substr(
      hostAt, hostEnd == std::wstring_view::npos ? std::wstring_view::npos
                                                 : hostEnd - hostAt);
  return host == L"stationhead.com" || host.ends_with(L".stationhead.com");
}

inline constexpr bool StationheadAdditionalNonPlaybackScriptUrl(
    std::wstring_view uriLower) {
  if (!StationheadHostUrl(uriLower)) return false;
  const size_t schemeEnd = uriLower.find(L"://");
  const size_t hostAt = schemeEnd + 3;
  const size_t pathAt = uriLower.find_first_of(L"/?#", hostAt);
  if (pathAt == std::wstring_view::npos || uriLower[pathAt] != L'/') return false;
  const size_t pathEnd = uriLower.find_first_of(L"?#", pathAt);
  const std::wstring_view path = uriLower.substr(
      pathAt, pathEnd == std::wstring_view::npos ? std::wstring_view::npos
                                                 : pathEnd - pathAt);
  if (!path.ends_with(L".js") && !path.ends_with(L".mjs")) return false;

  constexpr std::wstring_view kNeedles[] = {
      L"notification", L"inbox", L"message-center", L"messages-panel",
      L"share-sheet", L"sharing", L"invite", L"referral", L"followers",
      L"following", L"follow-modal", L"discover", L"discovery",
      L"social-feed", L"activity-feed", L"audience-modal",
      L"listener-modal", L"listeners-modal",
  };
  for (const std::wstring_view needle : kNeedles) {
    if (path.find(needle) != std::wstring_view::npos) return true;
  }
  return false;
}

static_assert(StationheadHostUrl(L"https://stationhead.com/c/buddies"));
static_assert(StationheadHostUrl(L"https://www.stationhead.com/c/buddies"));
static_assert(!StationheadHostUrl(L"https://stationhead.com.example/c/buddies"));
static_assert(StationheadAdditionalNonPlaybackScriptUrl(
    L"https://www.stationhead.com/_next/static/chunks/notifications-panel.123.js"));
static_assert(StationheadAdditionalNonPlaybackScriptUrl(
    L"https://stationhead.com/assets/share-sheet-a1b2.mjs?build=2"));
static_assert(!StationheadAdditionalNonPlaybackScriptUrl(
    L"https://www.stationhead.com/assets/player-runtime-a1b2.js"));
static_assert(!StationheadAdditionalNonPlaybackScriptUrl(
    L"https://www.stationhead.com/assets/auth-session-a1b2.js"));
static_assert(!StationheadAdditionalNonPlaybackScriptUrl(
    L"https://www.stationhead.com/assets/queue-realtime-a1b2.js"));
static_assert(!StationheadAdditionalNonPlaybackScriptUrl(
    L"https://cdn.example.com/assets/notifications-panel.js"));

// Locates the current Start Listening-like control, if any, and returns its
// clickable center point as {x, y}, or null if there is nothing to click
// right now (already playing, or no eligible control visible). Called
// on-demand from native code immediately before dispatching a click, so the
// coordinates it returns are always fresh - there is no page-scheduled scan
// loop whose captured position could go stale while a message travels back
// to native code.
inline std::wstring StationheadLocateStartButtonScript() {
  static constexpr wchar_t kScript[] = LR"JS(
(() => {
  const host = String(location.hostname || '').toLowerCase();
  if (host !== 'stationhead.com' && !host.endsWith('.stationhead.com')) return null;
  const startPattern = /\b(start|join|resume|continue)\s+(listening|station|show|room)\b|\blisten\s+(now|live)\b|^(continue|続ける|続行|次へ)$/i;
  const normalize = value => String(value || '').replace(/\s+/g, ' ').trim();
  const labelOf = element => normalize([
    element?.innerText,
    element?.getAttribute?.('aria-label'),
    element?.textContent,
    element?.getAttribute?.('title'),
    element?.getAttribute?.('value'),
    element?.getAttribute?.('data-testid')
  ].filter(Boolean).join(' '));
  const playing = () => {
    if (typeof window.__homepanelAudioPlaying === 'boolean') return window.__homepanelAudioPlaying;
    if (navigator.mediaSession?.playbackState === 'playing') return true;
    return Array.from(document.querySelectorAll('audio,video')).some(element =>
      !element.paused && !element.ended && element.readyState >= 2);
  };
  if (!document.body || playing()) return null;
  const selector = "button,[role='button'],a,input[type='button'],input[type='submit'],[aria-label],[data-testid],[tabindex]";
  for (const element of document.querySelectorAll(selector)) {
    if (!(element instanceof HTMLElement) || !element.isConnected) continue;
    if (element.disabled || element.getAttribute('aria-disabled') === 'true' ||
        element.getAttribute('aria-hidden') === 'true') continue;
    if (!startPattern.test(labelOf(element))) continue;
    if (element.matches('audio,video') || element.querySelector?.('audio,video')) continue;
    const rect = element.getBoundingClientRect();
    if (!rect || rect.width <= 2 || rect.height <= 2) continue;
    const style = getComputedStyle(element);
    if (style.display === 'none' || style.visibility === 'hidden' ||
        Number(style.opacity || 1) <= 0 || style.pointerEvents === 'none') continue;
    const x = rect.left + rect.width / 2;
    const y = rect.top + rect.height / 2;
    if (x < 0 || y < 0 || x >= innerWidth || y >= innerHeight) continue;
    const hit = document.elementFromPoint(x, y);
    if (!hit || (hit !== element && !element.contains(hit))) continue;
    return { x, y };
  }
  return null;
})()
)JS";
  return kScript;
}

inline bool ParseStationheadLocateButtonResult(const std::wstring& resultJson,
                                               double& x,
                                               double& y) {
  if (resultJson.empty() || resultJson == L"null") return false;
  try {
    const auto root = winrt::Windows::Data::Json::JsonObject::Parse(resultJson);
    if (!root.HasKey(L"x") || !root.HasKey(L"y")) return false;
    x = root.GetNamedNumber(L"x");
    y = root.GetNamedNumber(L"y");
  } catch (...) {
    return false;
  }
  return std::isfinite(x) && std::isfinite(y) && x >= 0.0 && y >= 0.0 &&
         x <= 10000.0 && y <= 10000.0;
}

inline void DispatchStationheadNativeClick(ICoreWebView2* webview,
                                           double x,
                                           double y,
                                           Logger& log,
                                           const wchar_t* roleTag) {
  if (!webview) return;
  std::wostringstream pressed;
  pressed << std::fixed << std::setprecision(2)
          << L"{\"type\":\"mousePressed\",\"x\":" << x
          << L",\"y\":" << y
          << L",\"button\":\"left\",\"buttons\":1,\"clickCount\":1}";
  ComPtr<ICoreWebView2> view = webview;
  view->CallDevToolsProtocolMethod(
      L"Input.dispatchMouseEvent", pressed.str().c_str(),
      Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
          [view, x, y, &log, roleTag](HRESULT pressedResult, LPCWSTR) -> HRESULT {
            if (FAILED(pressedResult)) {
              std::wostringstream detail;
              detail << L"Stationhead " << roleTag
                     << L" native click mousePressed dispatch failed 0x"
                     << std::hex << static_cast<unsigned long>(pressedResult);
              log.Warn(detail.str());
            }
            if (!view) return S_OK;
            std::wostringstream released;
            released << std::fixed << std::setprecision(2)
                     << L"{\"type\":\"mouseReleased\",\"x\":" << x
                     << L",\"y\":" << y
                     << L",\"button\":\"left\",\"buttons\":0,\"clickCount\":1}";
            view->CallDevToolsProtocolMethod(
                L"Input.dispatchMouseEvent", released.str().c_str(),
                Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
                    [&log, roleTag](HRESULT releasedResult, LPCWSTR) -> HRESULT {
                      if (FAILED(releasedResult)) {
                        std::wostringstream detail;
                        detail << L"Stationhead " << roleTag
                               << L" native click mouseReleased dispatch failed 0x"
                               << std::hex << static_cast<unsigned long>(releasedResult);
                        log.Warn(detail.str());
                      }
                      return S_OK;
                    }).Get());
            return S_OK;
          }).Get());
}

inline void ApplyStationheadAdditionalScriptBlocking(
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
            if (!StationheadAdditionalNonPlaybackScriptUrl(uriLower)) return S_OK;

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
