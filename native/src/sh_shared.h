#pragma once
#include "common.h"
#include "config.h"

namespace hp {





inline int64_t StationheadReloadIntervalMs(int intervalMinutes) noexcept {
  return intervalMinutes > 0 ? static_cast<int64_t>(intervalMinutes) * 60'000 : 0;
}

// Shared "click Start Listening" automation injected into both Stationhead
// WebViews at document creation. The two players differ only in the guard
// global (so the script runs once per window kind) and the postMessage
// prefix their native message handlers listen for.
inline std::wstring StationheadAutoplayScript(const wchar_t* globalName,
                                              const wchar_t* messagePrefix) {
  static constexpr wchar_t kTemplate[] = LR"JS(
(() => {
  const host = String(location.hostname || '').toLowerCase();
  if (host !== 'stationhead.com' && !host.endsWith('.stationhead.com')) return;
  if (window.{{GLOBAL}}) return;
  const nativeTimeout = window.setTimeout.bind(window);
  const NativeMutationObserver = window.MutationObserver;
  const normalize = value => String(value || '').replace(/\s+/g, ' ').trim();
  const selector = "button,[role='button'],a,input[type='button'],input[type='submit'],[aria-label],[data-testid],[tabindex]";
  const startPattern = /\b(start|join|resume|continue)\s+(listening|station|show|room)\b|\blisten\s+(now|live)\b|^(continue|続ける|続行|次へ)$/i;
  const loginPattern = /^(log\s*in|sign\s*in|login)(?:\s+.*)?$/i;
  let observer = null;
  let scanQueued = false;
  let scanTimer = 0;
  let attempts = 0;
  let retryAt = 0;
  let loginReported = false;
  let lastTargetSignature = '';
  let lastPlaying = null;
  const observedAt = Date.now();
  const labelOf = element => [
    element?.innerText,
    element?.getAttribute?.('aria-label'),
    element?.textContent,
    element?.getAttribute?.('title'),
    element?.getAttribute?.('value'),
    element?.getAttribute?.('data-testid')
  ].map(normalize).find(Boolean) || '';
  const visible = element => {
    if (!element || element.disabled || element.getAttribute?.('aria-disabled') === 'true' ||
        element.getAttribute?.('aria-hidden') === 'true') return false;
    const rect = element.getBoundingClientRect?.();
    if (!rect || rect.width <= 2 || rect.height <= 2) return false;
    const style = getComputedStyle(element);
    return style.display !== 'none' && style.visibility !== 'hidden' &&
      Number(style.opacity || 1) > 0 && style.pointerEvents !== 'none';
  };
  const playing = () => {
    if (navigator.mediaSession?.playbackState === 'playing') return true;
    return Array.from(document.querySelectorAll('audio,video')).some(element =>
      !element.paused && !element.ended && element.readyState >= 2);
  };
  const publishAudio = () => {
    const current = playing();
    if (current === lastPlaying) return current;
    lastPlaying = current;
    try { window.chrome?.webview?.postMessage(current ? '{{PREFIX}}-playing' : '{{PREFIX}}-stopped'); } catch (_) {}
    return current;
  };
  const scan = () => {
    scanQueued = false;
    scanTimer = 0;
    const ready = document.readyState !== 'loading' && !!document.body;
    const isPlaying = publishAudio();
    if (!ready || !document.body) return;
    if (isPlaying) {
      observer?.disconnect?.();
      observer = null;
      return;
    }
    let start = null;
    let login = false;
    for (const element of document.querySelectorAll(selector)) {
      if (!visible(element)) continue;
      const label = labelOf(element);
      if (!start && startPattern.test(label)) start = element;
      if (!login && loginPattern.test(label)) login = true;
      if (start && login) break;
    }
    if (start && attempts < 2 && Date.now() >= retryAt) {
      const target = start.closest?.("button,[role='button'],a,input[type='button'],input[type='submit'],[tabindex]") || start;
      const rect = target.getBoundingClientRect();
      const signature = `${labelOf(target)}:${Math.round(rect.left)}:${Math.round(rect.top)}`;
      if (signature !== lastTargetSignature) {
        lastTargetSignature = signature;
        attempts = 0;
        retryAt = 0;
      }
      attempts += 1;
      retryAt = Date.now() + 1500;
      try { window.chrome?.webview?.postMessage('{{PREFIX}}-start-attempted'); } catch (_) {}
      try { target.click?.(); } catch (_) {}
      if (attempts < 2) nativeTimeout(schedule, 1500);
    } else if (!start && login && !loginReported && Date.now() - observedAt >= 15000) {
      loginReported = true;
      try { window.chrome?.webview?.postMessage('{{PREFIX}}-login-required'); } catch (_) {}
    }
  };
  const schedule = () => {
    if (scanQueued) return;
    scanQueued = true;
    scanTimer = nativeTimeout(scan, 250);
  };
  const relevant = record => {
    if (record.type === 'attributes') return Boolean(record.target?.matches?.(selector) || record.target?.closest?.(selector));
    if (record.type === 'characterData') return Boolean(record.target?.parentElement?.closest?.(selector));
    return [...(record.addedNodes || []), ...(record.removedNodes || [])].some(node =>
      node instanceof Element && (node.matches?.(selector) || node.querySelector?.(selector)));
  };
  window.{{GLOBAL}} = { scan: schedule };
  if (NativeMutationObserver) {
    observer = new NativeMutationObserver(records => { if (records.some(relevant)) schedule(); });
    observer.observe(document, { childList: true, subtree: true });
  }
  for (const eventName of ['play','playing','canplay','pause','ended','stalled','waiting','error']) {
    document.addEventListener(eventName, publishAudio, true);
  }
  document.addEventListener('DOMContentLoaded', schedule, { once: true });
  window.addEventListener('load', schedule, { once: true });
  schedule();
  nativeTimeout(schedule, 15000);
})()
)JS";
  const auto replaceAll = [](std::wstring text, std::wstring_view from, std::wstring_view to) {
    for (size_t at = text.find(from); at != std::wstring::npos; at = text.find(from, at + to.size())) {
      text.replace(at, from.size(), to);
    }
    return text;
  };
  return replaceAll(replaceAll(kTemplate, L"{{GLOBAL}}", globalName), L"{{PREFIX}}", messagePrefix);
}

inline std::wstring StationheadVolumeScript(int percent) {
  std::wostringstream script;
  script << L"(() => { const v=" << percent << L"/100;"
         << L"window.__homepanelStationheadVolume=v;"
         << L"const apply=e=>{if(!e||e.__homepanelVolume===v)return;"
         << L"try{e.volume=v;e.defaultMuted=v<=0;e.muted=v<=0?true:false;e.__homepanelVolume=v;}catch(_){}};"
         << L"const applyAll=(root=document)=>{for(const e of root.querySelectorAll?.('audio,video')||[])apply(e);"
         << L"if(root?.matches?.('audio,video'))apply(root);};"
         << L"applyAll();"
         << L"if(!window.__homepanelStationheadVolumeObserver){"
         << L"window.__homepanelStationheadVolumeApply=()=>applyAll();"
         << L"window.__homepanelStationheadVolumeObserver=new MutationObserver(records=>{"
         << L"for(const record of records){for(const node of record.addedNodes||[]){"
         << L"if(node instanceof Element){applyAll(node);}}}});"
         << L"window.__homepanelStationheadVolumeObserver.observe(document,{childList:true,subtree:true});"
         << L"document.addEventListener('play',event=>apply(event.target),true);"
         << L"document.addEventListener('loadedmetadata',event=>apply(event.target),true);"
         << L"} return true; })()";
  return script.str();
}

// Injected once per document so a later ExecuteScript call can authenticate
// as the logged-in account without ever touching where the page keeps its
// token: it transparently observes the Authorization header the page's own
// fetch()/XHR calls already send to *.stationhead.com and caches the latest
// one. The wrapped fetch/XHR behavior is otherwise untouched.
inline std::wstring StationheadAuthCaptureScript() {
  static constexpr wchar_t kScript[] = LR"JS(
(() => {
  const host = String(location.hostname || '').toLowerCase();
  if (host !== 'stationhead.com' && !host.endsWith('.stationhead.com')) return;
  if (window.__homepanelAuthCapture) return;
  window.__homepanelAuthCapture = true;
  window.__homepanelStationheadAuthHeaders = null;
  const relevant = url => /(^|\.)stationhead\.com/i.test(String(url || ''));
  const capture = (url, getHeader) => {
    if (!relevant(url)) return;
    const auth = getHeader('authorization');
    if (!auth) return;
    window.__homepanelStationheadAuthHeaders = {
      authorization: auth,
      'sth-device-uid': getHeader('sth-device-uid') || '',
      'app-platform': getHeader('app-platform') || 'web',
      'app-version': getHeader('app-version') || '1.0.0',
    };
  };
  const nativeFetch = window.fetch ? window.fetch.bind(window) : null;
  if (nativeFetch) {
    window.fetch = function(input, init) {
      try {
        const headers = new Headers((init && init.headers) || (input && input.headers) || {});
        const url = typeof input === 'string' ? input : (input && input.url) || '';
        capture(url, name => headers.get(name));
      } catch (_) {}
      return nativeFetch(input, init);
    };
  }
  const NativeXhr = window.XMLHttpRequest;
  if (NativeXhr) {
    const nativeOpen = NativeXhr.prototype.open;
    const nativeSetHeader = NativeXhr.prototype.setRequestHeader;
    const nativeSend = NativeXhr.prototype.send;
    NativeXhr.prototype.open = function(method, url, ...rest) {
      this.__homepanelUrl = url;
      this.__homepanelHeaders = {};
      return nativeOpen.call(this, method, url, ...rest);
    };
    NativeXhr.prototype.setRequestHeader = function(name, value) {
      try { this.__homepanelHeaders[String(name).toLowerCase()] = value; } catch (_) {}
      return nativeSetHeader.call(this, name, value);
    };
    NativeXhr.prototype.send = function(...args) {
      try { capture(this.__homepanelUrl, name => this.__homepanelHeaders?.[name]); } catch (_) {}
      return nativeSend.apply(this, args);
    };
  }
})()
)JS";
  return kScript;
}

// Uses the Authorization header cached by StationheadAuthCaptureScript to
// fetch the logged-in account's per-day listening activity. ExecuteScript
// does not await a returned JavaScript Promise, so publish the asynchronous
// result through WebView2's message channel instead.
inline std::wstring StationheadStreakStatsScript(int channelId) {
  std::wostringstream script;
  script << LR"JS(
(() => {
  const headers = window.__homepanelStationheadAuthHeaders;
  if (!headers || !headers.authorization) return { ok: false, error: 'no-auth' };
  fetch('https://production1.stationhead.com/me/channel/)JS"
         << channelId << LR"JS(/streakStats', {
    headers: Object.assign({ accept: 'application/json' }, headers),
  }).then(r => r.ok ? r.json() : Promise.reject(new Error('http-' + r.status)))
    .then(data => {
      try { window.chrome?.webview?.postMessage({ type: 'stationhead-play-stats', data }); } catch (_) {}
    })
    .catch(() => {});
  return true;
})()
)JS";
  return script.str();
}

// ASCII-lowercases a URI for case-insensitive substring matching without
// pulling in locale-aware towlower; request paths such as "chatHistory" mix
// case while hostnames are already lowercase.
inline std::wstring StationheadLowerAscii(const wchar_t* text) {
  std::wstring lower;
  if (text) {
    for (const wchar_t* p = text; *p; ++p) {
      const wchar_t c = *p;
      lower.push_back((c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c);
    }
  }
  return lower;
}

// True for requests that a background audio-only Stationhead window never
// needs: third-party analytics/crash/marketing/push telemetry, and the
// social surfaces of the Stationhead API (chat, tipping, emoji, trending
// threads). Derived from an actual startup network capture; the audio
// stream and the core playback REST endpoints (/timestamp,
// /pusher/presenceAuth, /channels/alias/*, /me/country) are left untouched.
// The Pusher realtime WebSocket is NOT handled here (WebSocket upgrades don't



inline bool StationheadRequestIsBlockable(const std::wstring& uriLower) {
  static constexpr const wchar_t* kNeedles[] = {

      L"firebaseinstallations.googleapis.com",
      L"firebaseremoteconfig.googleapis.com",
      L"firebase.googleapis.com",
      L"firebaselogging.googleapis.com",
      L"firestore.googleapis.com",
      L"crashlytics",
      L"amplitude.com",
      L"google-analytics.com",
      L"analytics.google.com",
      L"googletagmanager.com",
      L"doubleclick.net",
      L"sentry.io",
      L"bugsnag.com",
      L"branch.io",
      L"segment.io",
      L"segment.com",
      L"mixpanel.com",
      L"hotjar.com",
      L"fullstory.com",
      L"appsflyer.com",
      L"adjust.com",
      L"braze.com",
      L"onesignal.com",
      L"intercom.io",
      L"clarity.ms",
      L"datadoghq",
      L"newrelic",
      L"nr-data.net",
      L"statsigapi.net",
      L"launchdarkly.com",
      L"googleadservices.com",
      L"adservice.google.com",
      L"facebook.com/tr",
      L"connect.facebook.net",
      L"twitter.com/i/",
      L"x.com/i/",
      L"tiktok.com",
      L"snapchat.com",
      L"pinterest.com",

      L"/chathistory",
      L"/streams",
      L"/tippingstatus",
      L"/posts/trending",
      L"/threads/",
      L"/tipping",
      L"/emoji",
      L"/gifts",
  };
  for (const wchar_t* needle : kNeedles) {
    if (uriLower.find(needle) != std::wstring::npos) return true;
  }
  return false;
}

// These authenticated API calls are only UI/account chrome. Keep them
// available during login and startup, then suppress their periodic refreshes
// once audio has proven that the station is fully joined.
inline bool StationheadRequestIsBlockableAfterPlayback(const std::wstring& uriLower) {
  static constexpr std::wstring_view kApiHost = L"production1.stationhead.com";
  const size_t schemeEnd = uriLower.find(L"://");
  if (schemeEnd == std::wstring::npos) return false;
  const size_t hostAt = schemeEnd + 3;
  size_t pathAt = uriLower.find_first_of(L"/?#", hostAt);
  if (pathAt == std::wstring::npos) pathAt = uriLower.size();
  if (uriLower.compare(hostAt, pathAt - hostAt, kApiHost) != 0 ||
      pathAt == uriLower.size() || uriLower[pathAt] != L'/') {
    return false;
  }
  const auto exactPath = [&](std::wstring_view path) {
    if (uriLower.compare(pathAt, path.size(), path) != 0) return false;
    const size_t end = pathAt + path.size();
    return end == uriLower.size() || uriLower[end] == L'?' || uriLower[end] == L'#';
  };
  return exactPath(L"/plus/status") || exactPath(L"/tos");
}

inline bool StationheadCorePlaybackRequest(const std::wstring& uriLower) {
  if (uriLower.empty()) return false;
  // The captured Pusher-compatible socket carries station/queue updates even
  // though it is hosted on a Stationhead domain rather than pusher.com.
  if (uriLower.find(L"://realtime-production.stationhead.com/app/") !=
      std::wstring::npos) {
    return true;
  }
  const bool stationhead = uriLower.find(L"stationhead.com") != std::wstring::npos;
  const bool spotify = uriLower.find(L"spotify") != std::wstring::npos ||
                       uriLower.find(L"scdn.co") != std::wstring::npos;
  if (!stationhead && !spotify) return false;
  if (uriLower.find(L"/timestamp") != std::wstring::npos ||
      uriLower.find(L"/pusher/presenceauth") != std::wstring::npos ||
      uriLower.find(L"/channels/alias/") != std::wstring::npos ||
      uriLower.find(L"/me/country") != std::wstring::npos) {
    return true;
  }
  return spotify &&
      (uriLower.find(L"audio") != std::wstring::npos ||
       uriLower.find(L"playback") != std::wstring::npos ||
       uriLower.find(L"gew") != std::wstring::npos);
}










inline void BlockStationheadRealtimeSockets(ICoreWebView2* webview) {
  if (!webview) return;
  webview->CallDevToolsProtocolMethod(L"Network.enable", L"{}", nullptr);
  webview->CallDevToolsProtocolMethod(
      L"Network.setBlockedURLs",
      L"{\"urls\":["
      L"\"*pusher.com*\",\"*pusherapp.com*\",\"*pusher.io*\","
      L"\"*google-analytics.com*\",\"*googletagmanager.com*\",\"*doubleclick.net*\","
      L"\"*amplitude.com*\",\"*segment.com*\",\"*segment.io*\","
      L"\"*clarity.ms*\",\"*datadoghq*\",\"*newrelic*\",\"*nr-data.net*\","
      L"\"*statsigapi.net*\",\"*launchdarkly.com*\","
      L"\"*facebook.com/tr*\",\"*connect.facebook.net*\",\"*twitter.com/i/*\",\"*x.com/i/*\""
      L"]}",
      nullptr);
}



















inline void ApplyStationheadResourceBlocking(ICoreWebView2Environment* environment,
                                              ICoreWebView2* webview,
                                              const StationheadConfig& config,
                                              std::atomic<bool>& armed,
                                              EventRegistrationToken& token) {
  if (!environment || !webview) return;
  webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_IMAGE);
  webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_FONT);
  webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_STYLESHEET);
  webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_MEDIA);
  webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_SCRIPT);
  webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_XML_HTTP_REQUEST);
  webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_FETCH);
  webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_TEXT_TRACK);
  webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_EVENT_SOURCE);
  webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_WEBSOCKET);
  webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_MANIFEST);
  webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_PING);
  webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_CSP_VIOLATION_REPORT);
  const bool blockImages = config.blockImagesAfterPlayback;
  const bool blockFonts = config.blockFontsAfterPlayback;
  ComPtr<ICoreWebView2Environment> env = environment;
  webview->add_WebResourceRequested(
      Callback<ICoreWebView2WebResourceRequestedEventHandler>(
          [env, &armed, blockImages, blockFonts](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
            if (!args) return S_OK;
            std::wstring lower;
            ComPtr<ICoreWebView2WebResourceRequest> request;
            if (SUCCEEDED(args->get_Request(&request)) && request) {
              LPWSTR uriRaw = nullptr;
              if (SUCCEEDED(request->get_Uri(&uriRaw)) && uriRaw) {
                lower = StationheadLowerAscii(uriRaw);
                CoTaskMemFree(uriRaw);
              }
            }
            const bool armedNow = armed.load(std::memory_order_relaxed);
            bool block = StationheadRequestIsBlockable(lower) ||
                         (armedNow && StationheadRequestIsBlockableAfterPlayback(lower));
            if (!block && (blockImages || blockFonts)) {
              COREWEBVIEW2_WEB_RESOURCE_CONTEXT context = COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL;
              if (SUCCEEDED(args->get_ResourceContext(&context))) {
                if (blockImages && context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_IMAGE) {
                  // Images are never required for background audio playback.
                  // Block them from the first navigation to avoid downloading
                  // Stationhead artwork and avatars before playback is armed.
                  block = true;
                } else if (blockFonts && context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_FONT) {
                  block = armedNow;
                } else if (context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_STYLESHEET) {
                  block = armedNow;
                } else if (context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_MEDIA) {
                  block = !lower.empty() &&
                          lower.find(L"stationhead.com") == std::wstring::npos &&
                          lower.find(L"spotify") == std::wstring::npos;
                } else if (context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_TEXT_TRACK ||
                           context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_MANIFEST ||
                           context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_CSP_VIOLATION_REPORT) {
                  block = true;
                } else if (context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_EVENT_SOURCE ||
                           context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_WEBSOCKET) {
                  block = !StationheadCorePlaybackRequest(lower);
                }
              }
            }
            if (block) {
              ComPtr<ICoreWebView2WebResourceResponse> response;
              if (SUCCEEDED(env->CreateWebResourceResponse(nullptr, 403, L"Blocked", L"", &response))) {
                args->put_Response(response.Get());
              }
            }
            return S_OK;
          }).Get(),
      &token);
  BlockStationheadRealtimeSockets(webview);
}
}
