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
         << L"window.__homepanelStationheadVolumeTimer=setInterval(window.__homepanelStationheadVolumeApply,5000);"
         << L"} return true; })()";
  return script.str();
}

// Blocks image/font network requests once a Stationhead WebView has
// confirmed playback (armed set true by the caller at that point), matching
// the blockImagesAfterPlayback/blockFontsAfterPlayback config flags' actual
// names (previously loaded from cloud config but never actually applied).
// The filter is registered from webview creation, but the handler only
// blocks once armed is true: the pre-playback "click Start Listening"
// automation depends on getBoundingClientRect() of on-page controls, and
// blocking images before that point can collapse icon-only buttons to zero
// size, breaking auto-play detection and leaving the window stuck visible.
// Shared by both the primary and secondary Stationhead players so the two
// windows apply the same rule the same way. The registered token must be
// removed via webview->remove_WebResourceRequested(token) when the webview
// is closed, and armed should be reset to false at that point too.
inline void ApplyStationheadResourceBlocking(ICoreWebView2Environment* environment,
                                              ICoreWebView2* webview,
                                              const StationheadConfig& config,
                                              std::atomic<bool>& armed,
                                              EventRegistrationToken& token) {
  if (!environment || !webview) return;
  if (!config.blockImagesAfterPlayback && !config.blockFontsAfterPlayback) return;
  if (config.blockImagesAfterPlayback) {
    webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_IMAGE);
  }
  if (config.blockFontsAfterPlayback) {
    webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_FONT);
  }
  ComPtr<ICoreWebView2Environment> env = environment;
  webview->add_WebResourceRequested(
      Callback<ICoreWebView2WebResourceRequestedEventHandler>(
          [env, &armed](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
            if (!args || !armed.load(std::memory_order_relaxed)) return S_OK;
            ComPtr<ICoreWebView2WebResourceResponse> response;
            if (SUCCEEDED(env->CreateWebResourceResponse(nullptr, 403, L"Blocked", L"", &response))) {
              args->put_Response(response.Get());
            }
            return S_OK;
          }).Get(),
      &token);
}
}  // namespace hp
