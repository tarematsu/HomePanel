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

inline std::wstring StationheadNativeStartClickBridgeScript() {
  static constexpr wchar_t kScript[] = LR"JS(
(() => {
  const host = String(location.hostname || '').toLowerCase();
  if (host !== 'stationhead.com' && !host.endsWith('.stationhead.com')) return;
  if (window.__homepanelStationheadNativeStartClickBridge) return;
  window.__homepanelStationheadNativeStartClickBridge = true;

  const originalClick = HTMLElement.prototype.click;
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
    if (navigator.mediaSession?.playbackState === 'playing') return true;
    return Array.from(document.querySelectorAll('audio,video')).some(element =>
      !element.paused && !element.ended && element.readyState >= 2);
  };
  const targetAtCenter = (element, rect) => {
    const x = rect.left + rect.width / 2;
    const y = rect.top + rect.height / 2;
    if (x < 0 || y < 0 || x >= innerWidth || y >= innerHeight) return null;
    const hit = document.elementFromPoint(x, y);
    if (!hit || (hit !== element && !element.contains(hit))) return null;
    return { x, y };
  };
  const eligible = element => {
    if (!(element instanceof HTMLElement) || !element.isConnected) return null;
    if (!startPattern.test(labelOf(element))) return null;
    if (element.matches('audio,video') || element.querySelector?.('audio,video')) return null;
    const rect = element.getBoundingClientRect?.();
    if (!rect || rect.width <= 2 || rect.height <= 2) return null;
    const style = getComputedStyle(element);
    if (style.display === 'none' || style.visibility === 'hidden' ||
        Number(style.opacity || 1) <= 0 || style.pointerEvents === 'none') return null;
    return targetAtCenter(element, rect);
  };

  let lastNativeAt = 0;
  let lastNativeSignature = '';
  HTMLElement.prototype.click = function(...args) {
    const point = playing() ? null : eligible(this);
    if (!point) return originalClick.apply(this, args);
    const signature = `${labelOf(this)}:${Math.round(point.x)}:${Math.round(point.y)}`;
    const now = Date.now();
    if (signature === lastNativeSignature && now - lastNativeAt < 1200) return;
    lastNativeSignature = signature;
    lastNativeAt = now;
    try {
      window.chrome?.webview?.postMessage(
        `stationhead-native-click:${point.x.toFixed(2)}:${point.y.toFixed(2)}`);
    } catch (_) {
      return originalClick.apply(this, args);
    }

    // The native dispatch above can be silently dropped (message-source
    // rejection, a failed CDP call, etc.) with no signal back to the page.
    // Fall back to a real DOM click if the target is still sitting there
    // unclicked a moment later, so a dropped native click doesn't leave
    // Start Listening permanently unresponsive.
    const target = this;
    window.setTimeout(() => {
      if (target.isConnected && eligible(target) && !playing()) {
        try { originalClick.apply(target, args); } catch (_) {}
      }
    }, 650);
  };
})()
)JS";
  return kScript;
}

inline bool ParseStationheadNativeClickMessage(std::wstring_view message,
                                               double& x,
                                               double& y) {
  constexpr std::wstring_view kPrefix = L"stationhead-native-click:";
  if (!message.starts_with(kPrefix)) return false;
  const std::wstring_view payload = message.substr(kPrefix.size());
  const size_t separator = payload.find(L':');
  if (separator == std::wstring_view::npos ||
      payload.find(L':', separator + 1) != std::wstring_view::npos) {
    return false;
  }
  try {
    size_t consumedX = 0;
    size_t consumedY = 0;
    const std::wstring xText(payload.substr(0, separator));
    const std::wstring yText(payload.substr(separator + 1));
    x = std::stod(xText, &consumedX);
    y = std::stod(yText, &consumedY);
    if (consumedX != xText.size() || consumedY != yText.size()) return false;
  } catch (...) {
    return false;
  }
  return std::isfinite(x) && std::isfinite(y) && x >= 0.0 && y >= 0.0 &&
         x <= 10000.0 && y <= 10000.0;
}

inline void DispatchStationheadNativeClick(ICoreWebView2* webview,
                                           double x,
                                           double y) {
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
          [view, x, y](HRESULT, LPCWSTR) -> HRESULT {
            if (!view) return S_OK;
            std::wostringstream released;
            released << std::fixed << std::setprecision(2)
                     << L"{\"type\":\"mouseReleased\",\"x\":" << x
                     << L",\"y\":" << y
                     << L",\"button\":\"left\",\"buttons\":0,\"clickCount\":1}";
            view->CallDevToolsProtocolMethod(
                L"Input.dispatchMouseEvent", released.str().c_str(),
                Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
                    [](HRESULT, LPCWSTR) -> HRESULT { return S_OK; }).Get());
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

  static const std::wstring nativeClickBridge =
      StationheadNativeStartClickBridgeScript();
  webview->AddScriptToExecuteOnDocumentCreated(nativeClickBridge.c_str(), nullptr);

  ComPtr<ICoreWebView2> clickView = webview;
  EventRegistrationToken ignoredMessageToken{};
  webview->add_WebMessageReceived(
      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [clickView](ICoreWebView2*,
                      ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            if (!args || !clickView) return S_OK;
            LPWSTR sourceRaw = nullptr;
            if (FAILED(args->get_Source(&sourceRaw)) || !sourceRaw) return S_OK;
            const std::wstring sourceLower = StationheadLowerAscii(sourceRaw);
            CoTaskMemFree(sourceRaw);
            if (!StationheadHostUrl(sourceLower)) return S_OK;

            LPWSTR raw = nullptr;
            if (FAILED(args->TryGetWebMessageAsString(&raw)) || !raw) return S_OK;
            const std::wstring message(raw);
            CoTaskMemFree(raw);
            double x = 0.0;
            double y = 0.0;
            if (ParseStationheadNativeClickMessage(message, x, y)) {
              DispatchStationheadNativeClick(clickView.Get(), x, y);
            }
            return S_OK;
          }).Get(),
      &ignoredMessageToken);

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
