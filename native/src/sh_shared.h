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
      L"{\"urls\":[\"*pusher.com*\",\"*pusherapp.com*\",\"*pusher.io*\"]}",
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
  webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_SCRIPT);
  webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_XML_HTTP_REQUEST);
  webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_FETCH);
  webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_PING);
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
