#pragma once
#include "common.h"
#include "config.h"

namespace hp {
// Shared "50-minute rule": periodic maintenance reload of a long-running
// Stationhead WebView to avoid unbounded session/memory growth. Both the
// primary and secondary players compute this the same way; callers keep
// their own gating conditions (e.g. only reload while audio is playing).
// A non-positive interval means the reload is disabled.
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
// raise WebResourceRequested); it is blocked separately at the network layer
// by BlockStationheadRealtimeSockets so no new chat/presence is received.
// Matched as case-insensitive substrings of the full URI.
inline bool StationheadRequestIsBlockable(const std::wstring& uriLower) {
  static constexpr const wchar_t* kNeedles[] = {
      // Analytics / remote-config / crash / marketing / push telemetry.
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
      // Non-playback Stationhead surfaces (chat / tipping / social threads / streams).
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

inline bool StationheadCorePlaybackRequest(const std::wstring& uriLower) {
  if (uriLower.empty()) return false;
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

// Realtime chat, presence and reactions are delivered over the Pusher
// WebSocket, which WebResourceRequested cannot intercept (WS upgrades don't
// raise the event). Block the Pusher socket hosts at the network layer via the
// DevTools Protocol so no new chat is received - at startup and for the whole
// life of the WebView, not just during the startup burst. The separate audio
// stream and the REST playback endpoints (served from stationhead.com, e.g.
// /pusher/presenceAuth, /channels/alias/*) are on different hosts and are
// unaffected. CDP domain state persists across navigations, so this only needs
// to run once per WebView (re-applied when a WebView is recreated).
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

// Strips resource requests from a Stationhead WebView down to what background
// audio playback needs. Two tiers:
//   * Analytics/social requests (StationheadRequestIsBlockable) are dropped
//     unconditionally, including at startup - they are never needed by the
//     "click Start Listening" automation or by playback, so cutting them
//     early reduces startup CPU/network/memory.
//   * Third-party images (artwork/avatars) are dropped from startup; same-site
//     stationhead.com images and all fonts are dropped only once armed is true
//     (playback confirmed). Blocking same-site images earlier can collapse
//     icon-only controls to zero size and break getBoundingClientRect()-based
//     auto-play detection, leaving the window stuck visible. The config flags
//     blockImagesAfterPlayback/blockFontsAfterPlayback still gate this tier.
//   * After playback is armed, additional stylesheets are blocked. The page's
//     already-loaded CSS remains in memory; this only prevents late UI/theme
//     fetches from waking the renderer/GPU for a hidden audio window.
//   * After playback is armed, late script/fetch/xhr traffic is reduced to
//     known core playback endpoints. This keeps the already-running page alive
//     while cutting background social/feature polling.
// Shared by the primary and secondary players so both apply the same rules.
// The token must be removed via remove_WebResourceRequested(token) on close,
// and armed reset to false at that point.
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
            bool block = StationheadRequestIsBlockable(lower);
            if (!block && (blockImages || blockFonts)) {
              COREWEBVIEW2_WEB_RESOURCE_CONTEXT context = COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL;
              if (SUCCEEDED(args->get_ResourceContext(&context))) {
                const bool armedNow = armed.load(std::memory_order_relaxed);
                if (blockImages && context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_IMAGE) {
                  // Third-party artwork/avatars are never the auto-play button
                  // and are dropped from startup. Same-site (stationhead)
                  // images are kept until playback is armed so icon-only
                  // controls keep their geometry for the coordinate-based
                  // auto-play click.
                  block = armedNow ||
                          (!lower.empty() && lower.find(L"stationhead.com") == std::wstring::npos);
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
                } else if (armedNow &&
                           (context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_SCRIPT ||
                            context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_XML_HTTP_REQUEST ||
                            context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_FETCH)) {
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
}  // namespace hp
