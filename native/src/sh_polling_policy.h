#pragma once
#include "logger.h"

// Keep the shared helpers available, but replace the policy-sensitive helpers
// with native-application implementations.
#define ApplyStationheadResourceBlocking ApplyStationheadResourceBlockingBase
#define StationheadAutoplayScript StationheadAutoplayScriptBase
#define StationheadVolumeScript StationheadVolumeScriptWithMutationObserver
#define StationheadApiPlayStatsScript StationheadApiPlayStatsScriptUnthrottled
#define StationheadAuthProbeScript StationheadAuthProbeScriptNetwork
#include "sh_shared.h"
#undef StationheadAuthProbeScript
#undef StationheadApiPlayStatsScript
#undef StationheadVolumeScript
#undef StationheadAutoplayScript
#undef ApplyStationheadResourceBlocking
#include "sh_script_blocking_extension.h"

namespace hp {

// Media elements already emit play/loadedmetadata when they become relevant.
// Use those events instead of retaining a document-wide MutationObserver that
// scans every Stationhead DOM insertion in both long-lived WebViews.
inline std::wstring StationheadVolumeScript(int percent) {
  std::wostringstream script;
  script << LR"JS(
(() => {
  const v = )JS"
         << percent
         << LR"JS( / 100;
  window.__homepanelStationheadVolume = v;

  // Disconnect the observer installed by older builds if this page survives an
  // in-place update/reload. New builds never create it.
  const legacyObserver = window.__homepanelStationheadVolumeObserver;
  if (legacyObserver) {
    try { legacyObserver.disconnect(); } catch (_) {}
    window.__homepanelStationheadVolumeObserver = null;
  }

  const apply = element => {
    if (!element?.matches?.('audio,video')) return;
    const current = Number(window.__homepanelStationheadVolume);
    if (!Number.isFinite(current) || element.__homepanelVolume === current) return;
    try {
      element.volume = current;
      element.defaultMuted = current <= 0;
      element.muted = current <= 0;
      element.__homepanelVolume = current;
    } catch (_) {}
  };
  const applyAll = () => {
    for (const element of document.querySelectorAll('audio,video')) apply(element);
  };
  window.__homepanelStationheadVolumeApply = applyAll;
  applyAll();

  // Keep one removable handler per page. Returning to the default 100% volume
  // now removes the listeners instead of leaving both long-lived WebViews with
  // callbacks that no longer perform useful work.
  let mediaEventHandler = window.__homepanelStationheadVolumeEventHandler;
  if (v !== 1) {
    if (!mediaEventHandler) {
      mediaEventHandler = event => apply(event.target);
      window.__homepanelStationheadVolumeEventHandler = mediaEventHandler;
    }
    if (!window.__homepanelStationheadVolumeEventsInstalled) {
      window.__homepanelStationheadVolumeEventsInstalled = true;
      document.addEventListener('play', mediaEventHandler, true);
      document.addEventListener('loadedmetadata', mediaEventHandler, true);
    }
  } else if (mediaEventHandler &&
             window.__homepanelStationheadVolumeEventsInstalled) {
    document.removeEventListener('play', mediaEventHandler, true);
    document.removeEventListener('loadedmetadata', mediaEventHandler, true);
    window.__homepanelStationheadVolumeEventsInstalled = false;
  }
  return true;
})()
)JS";
  return script.str();
}

// Block independently named social/UI JavaScript chunks before WebView2 sends
// their network requests. Keep the match limited to .js/.mjs resources so API
// routes such as /chathistory remain handled by the existing request policy and
// playback/authentication resources are not caught by a generic word match.
inline constexpr bool StationheadNonPlaybackScriptUrl(std::wstring_view uriLower) {
  const size_t pathEnd = uriLower.find_first_of(L"?#");
  const std::wstring_view path = uriLower.substr(
      0, pathEnd == std::wstring_view::npos ? uriLower.size() : pathEnd);
  if (!path.ends_with(L".js") && !path.ends_with(L".mjs")) return false;

  constexpr std::wstring_view kNonPlaybackScriptNeedles[] = {
      L"chat", L"comment", L"gift", L"tipping", L"trending", L"thread",
      L"reaction", L"emoji", L"listeners", L"audience", L"leaderboard",
      L"onboarding", L"walkthrough", L"tutorial", L"survey", L"feedback",
      L"rating-prompt", L"review-prompt", L"achievement", L"badge-modal",
      L"milestone", L"moderation-panel", L"search-modal", L"explore-panel",
      L"apple-music", L"musickit", L"connect-apple", L"music-service",
      L"service-picker",
  };
  for (const std::wstring_view needle : kNonPlaybackScriptNeedles) {
    if (path.find(needle) != std::wstring_view::npos) return true;
  }
  return false;
}

static_assert(StationheadNonPlaybackScriptUrl(
    L"https://www.stationhead.com/assets/chat-panel-a1b2.js"));
static_assert(StationheadNonPlaybackScriptUrl(
    L"https://www.stationhead.com/_next/static/chunks/listeners-modal.123.js?build=1"));
static_assert(StationheadNonPlaybackScriptUrl(
    L"https://www.stationhead.com/_next/static/chunks/onboarding-walkthrough.123.js"));
static_assert(StationheadNonPlaybackScriptUrl(
    L"https://www.stationhead.com/assets/review-prompt-a1b2.js"));
static_assert(StationheadNonPlaybackScriptUrl(
    L"https://www.stationhead.com/_next/static/chunks/apple-music-connect.123.js"));
static_assert(StationheadNonPlaybackScriptUrl(
    L"https://www.stationhead.com/assets/musickit-loader-a1b2.js"));
static_assert(!StationheadNonPlaybackScriptUrl(
    L"https://www.stationhead.com/assets/player-runtime-a1b2.js"));
static_assert(!StationheadNonPlaybackScriptUrl(
    L"https://production1.stationhead.com/chathistory"));

inline void ApplyStationheadNonPlaybackScriptBlocking(
    ICoreWebView2Environment* environment,
    ICoreWebView2* webview) {
  if (!environment || !webview) return;
  webview->AddWebResourceRequestedFilter(
      L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_SCRIPT);

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
            if (!StationheadNonPlaybackScriptUrl(uriLower)) return S_OK;

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

inline void ApplyStationheadResourceBlocking(
    ICoreWebView2Environment* environment,
    ICoreWebView2* webview,
    const StationheadConfig& config,
    std::atomic<bool>& armed,
    EventRegistrationToken& token) {
  ApplyStationheadResourceBlockingBase(
      environment, webview, config, armed, token);
  ApplyStationheadNonPlaybackScriptBlocking(environment, webview);
  ApplyStationheadAdditionalScriptBlocking(environment, webview);
}

// Pusher carries both track transitions and live social traffic. The socket
// must remain connected for immediate next-track playback. Standalone social
// JavaScript chunks are blocked above before download; this DOM suppression is
// retained only as a fallback when Stationhead bundles presentation code into a
// shared playback chunk that cannot safely be rejected wholesale. It runs at
// document creation for both Stationhead windows and never touches login,
// Start Listening, Spotify authorization, or audio/video elements.
inline std::wstring StationheadAudioOnlyUiScript() {
  static constexpr wchar_t kScript[] = LR"JS(
(() => {
  const host = String(location.hostname || '').toLowerCase();
  if (host !== 'stationhead.com' && !host.endsWith('.stationhead.com')) return;
  if (window.__homepanelStationheadAudioOnlyUi) return;
  window.__homepanelStationheadAudioOnlyUi = true;

  const nativeTimeout = window.setTimeout.bind(window);
  const nativeClearTimeout = window.clearTimeout.bind(window);
  const hiddenAttribute = 'data-homepanel-audio-only-hidden';
  const styleId = 'homepanel-stationhead-audio-only';
  const safeSelector = [
    '[data-testid*="chat" i]', '[id*="chat" i]', '[aria-label*="chat" i]',
    '[data-testid*="comment" i]', '[id*="comment" i]', '[aria-label*="comment" i]',
    '[data-testid*="gift" i]', '[id*="gift" i]', '[aria-label*="gift" i]',
    '[data-testid*="tipping" i]', '[id*="tipping" i]', '[aria-label*="tipping" i]',
    '[data-testid*="trending" i]', '[id*="trending" i]', '[aria-label*="trending" i]',
    '[data-testid*="thread" i]', '[id*="thread" i]', '[aria-label*="thread" i]',
    '[data-testid*="reaction" i]', '[id*="reaction" i]', '[aria-label*="reaction" i]',
    '[data-testid*="emoji" i]', '[id*="emoji" i]', '[aria-label*="emoji" i]',
    'a[href*="/chat" i]', 'a[href*="/gift" i]', 'a[href*="/thread" i]'
  ].join(',');
  const auxiliarySelector = [
    '[data-testid*="listener" i]', '[id*="listener" i]', '[aria-label*="listener" i]',
    '[data-testid*="audience" i]', '[id*="audience" i]', '[aria-label*="audience" i]'
  ].join(',');
  const labelSelector = 'button,[role="button"],[role="tab"],h1,h2,h3,[aria-label],[data-testid]';
  const nonPlaybackPattern = /\b(chat|comments?|listeners?|audience|gifts?|tipping|trending|threads?|reactions?|emoji)\b|チャット|コメント|リスナー|視聴者|ギフト|投げ銭|トレンド|スレッド|リアクション|絵文字/i;
  const protectedPattern = /\b(start listening|listen now|join station|join room|resume|continue|play|pause|volume|mute|spotify|log in|sign in|login|connect)\b|視聴を開始|再生|一時停止|音量|ミュート|ログイン|接続|続ける/i;
  const normalize = value => String(value || '').replace(/\s+/g, ' ').trim();
  const labelOf = element => normalize([
    element?.getAttribute?.('aria-label'),
    element?.getAttribute?.('data-testid'),
    element?.getAttribute?.('title'),
    element?.innerText,
    element?.textContent,
  ].filter(Boolean).join(' '));
  const protectedNode = element => {
    if (!(element instanceof Element)) return true;
    if (element.matches('audio,video') || element.querySelector?.('audio,video')) return true;
    return protectedPattern.test(labelOf(element));
  };
  const hide = (element, expand = false) => {
    if (!(element instanceof Element) || protectedNode(element)) return;
    let target = element;
    if (expand) {
      const container = element.closest?.('aside,[role="tabpanel"],[role="dialog"]');
      if (container && container !== document.body && container !== document.documentElement &&
          !protectedNode(container)) {
        target = container;
      }
    }
    const tag = String(target.tagName || '').toLowerCase();
    if (target === document.body || target === document.documentElement || tag === 'main') return;
    target.setAttribute(hiddenAttribute, 'true');
  };
  const matching = (root, selector) => {
    const output = [];
    if (root instanceof Element && root.matches(selector)) output.push(root);
    for (const element of root?.querySelectorAll?.(selector) || []) output.push(element);
    return output;
  };
  const scan = root => {
    for (const element of matching(root, safeSelector)) hide(element, true);
    for (const element of matching(root, auxiliarySelector)) hide(element, false);
    for (const element of matching(root, labelSelector)) {
      const label = labelOf(element);
      if (!label || protectedPattern.test(label) || !nonPlaybackPattern.test(label)) continue;
      hide(element, element.matches('h1,h2,h3,[role="tab"]'));
    }
  };
  const installStyle = () => {
    if (document.getElementById(styleId)) return true;
    const root = document.head || document.documentElement;
    if (!root) return false;
    const style = document.createElement('style');
    style.id = styleId;
    style.textContent = `${safeSelector},[${hiddenAttribute}="true"]{display:none!important;visibility:hidden!important;pointer-events:none!important;content-visibility:hidden!important;contain:strict!important}`;
    root.appendChild(style);
    return true;
  };

  const pending = new Set();
  let flushTimer = 0;
  let observer = null;
  let rescanOnVisible = false;
  const pageHidden = () => document.visibilityState === 'hidden';
  const flush = () => {
    flushTimer = 0;
    if (pageHidden()) {
      pending.clear();
      rescanOnVisible = true;
      return;
    }
    for (const root of pending) {
      if (root === document || root?.isConnected) scan(root);
    }
    pending.clear();
  };
  const schedule = root => {
    if (pageHidden()) {
      rescanOnVisible = true;
      return;
    }
    if (root && !pending.has(document)) {
      if (pending.size >= 24) {
        pending.clear();
        pending.add(document);
      } else {
        pending.add(root);
      }
    }
    if (flushTimer) return;
    flushTimer = nativeTimeout(flush, 80);
  };
  const detachObserver = () => {
    observer?.disconnect?.();
    observer = null;
    window.__homepanelStationheadAudioOnlyUiObserver = null;
    pending.clear();
    if (flushTimer) {
      nativeClearTimeout(flushTimer);
      flushTimer = 0;
    }
  };
  const attachObserver = () => {
    if (observer || pageHidden() || !document.documentElement) return;
    observer = new MutationObserver(records => {
      for (const record of records) {
        if (record.type === 'attributes') {
          schedule(record.target);
          continue;
        }
        for (const node of record.addedNodes || []) {
          if (node instanceof Element) schedule(node);
        }
      }
    });
    observer.observe(document.documentElement, {
      childList: true,
      subtree: true,
      attributes: true,
      attributeFilter: ['id', 'data-testid', 'aria-label', 'role', 'href'],
    });
    window.__homepanelStationheadAudioOnlyUiObserver = observer;
  };
  const resumeObserver = () => {
    if (pageHidden() || !document.documentElement) return;
    installStyle();
    if (rescanOnVisible) {
      rescanOnVisible = false;
      scan(document);
    }
    attachObserver();
  };
  const pauseObserver = () => {
    rescanOnVisible = true;
    detachObserver();
  };
  const start = () => {
    if (!document.documentElement) return;
    installStyle();
    scan(document);
    if (pageHidden()) {
      rescanOnVisible = true;
      return;
    }
    attachObserver();
  };
  document.addEventListener('visibilitychange', () => {
    if (pageHidden()) pauseObserver();
    else resumeObserver();
  });
  if (document.documentElement) start();
  else document.addEventListener('DOMContentLoaded', start, { once: true });
})()
)JS";
  return kScript;
}

// Finding and clicking the Start Listening control now happens entirely on
// the native side (see StationheadLocateStartButtonScript and
// StationheadPlayer::AttemptNativeStartClick): the page only reports that a
// candidate control is visible, and native re-locates it and dispatches a
// trusted CDP click at click-time so a page reflow between detection and
// dispatch can never leave the click aimed at a stale position.
inline std::wstring StationheadAutoplayScript(const wchar_t* globalName,
                                               const wchar_t* messagePrefix) {
  std::wstring script = StationheadAudioOnlyUiScript();
  script.push_back(L'\n');
  script.append(StationheadBlankPageRecoveryScript());
  script.push_back(L'\n');
  script.append(StationheadAutoplayScriptBase(globalName, messagePrefix));
  return script;
}

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
