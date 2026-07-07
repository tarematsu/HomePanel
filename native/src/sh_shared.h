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

// Blocks image/font network requests for the lifetime of a Stationhead
// WebView starting from webview creation, instead of only after playback
// begins. These are long-running, audio-only kiosk WebViews, so images and
// fonts are pure bandwidth/CPU overhead. Controlled by the existing
// blockImagesAfterPlayback/blockFontsAfterPlayback config flags (previously
// loaded from cloud config but never actually applied). Shared by both the
// primary and secondary Stationhead players so the two windows apply the
// same rule the same way. The registered token must be removed via
// webview->remove_WebResourceRequested(token) when the webview is closed.
inline void ApplyStationheadResourceBlocking(ICoreWebView2Environment* environment,
                                              ICoreWebView2* webview,
                                              const StationheadConfig& config,
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
          [env](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
            if (!args) return S_OK;
            ComPtr<ICoreWebView2WebResourceResponse> response;
            if (SUCCEEDED(env->CreateWebResourceResponse(nullptr, 403, L"Blocked", L"", &response))) {
              args->put_Response(response.Get());
            }
            return S_OK;
          }).Get(),
      &token);
}
}  // namespace hp
