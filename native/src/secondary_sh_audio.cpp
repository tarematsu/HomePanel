#include "secondary_sh.h"

namespace hp {
void SecondaryStationheadPlayer::SetMuted(bool muted) noexcept {
  audioMuted_.store(muted, std::memory_order_relaxed);
  ApplyAudioState();
}

bool SecondaryStationheadPlayer::Muted() const noexcept {
  return audioMuted_.load(std::memory_order_relaxed);
}

void SecondaryStationheadPlayer::EnsureMuted() noexcept {
  SetMuted(true);
}

void SecondaryStationheadPlayer::EnsureAudioState() noexcept {
  ApplyAudioState();
}

void SecondaryStationheadPlayer::SetVolume(double volume) noexcept {
  audioVolume_.store(std::clamp(volume, 0.0, 1.0), std::memory_order_relaxed);
  ApplyVolume();
}

double SecondaryStationheadPlayer::Volume() const noexcept {
  return audioVolume_.load(std::memory_order_relaxed);
}

void SecondaryStationheadPlayer::ApplyAudioState() noexcept {
  const BOOL muted = audioMuted_.load(std::memory_order_relaxed) ? TRUE : FALSE;
  const auto apply = [muted](const ComPtr<ICoreWebView2>& view) noexcept {
    if (!view) return;
    ComPtr<ICoreWebView2_8> audio;
    if (SUCCEEDED(view.As(&audio)) && audio) audio->put_IsMuted(muted);
  };
  apply(webview_);
  apply(authWebview_);
  ApplyVolume();
  EnsureDistinctBrowserIdentity();
}

void SecondaryStationheadPlayer::ApplyVolume() const noexcept {
  const int percent = std::clamp(static_cast<int>(audioVolume_.load(std::memory_order_relaxed) * 100.0 + 0.5), 0, 100);
  const auto apply = [percent](const ComPtr<ICoreWebView2>& view) noexcept {
    if (!view) return;
    std::wostringstream script;
    script << L"(() => { const v=" << percent << L"/100;"
           << L"window.__homepanelStationheadVolume=v;"
           << L"const apply=e=>{try{e.volume=v;e.defaultMuted=v<=0;e.muted=v<=0?true:false;}catch(_){}};"
           << L"for(const e of document.querySelectorAll('audio,video'))apply(e);"
           << L"if(!window.__homepanelStationheadVolumeObserver){"
           << L"window.__homepanelStationheadVolumeObserver=new MutationObserver(()=>{"
           << L"const x=Number(window.__homepanelStationheadVolume ?? 1);"
           << L"for(const e of document.querySelectorAll('audio,video')){try{e.volume=x;e.defaultMuted=x<=0;e.muted=x<=0?true:false;}catch(_){}}"
           << L"});"
           << L"window.__homepanelStationheadVolumeObserver.observe(document,{childList:true,subtree:true});"
           << L"window.__homepanelStationheadVolumeApply=()=>{const x=Number(window.__homepanelStationheadVolume ?? 1);for(const e of document.querySelectorAll('audio,video')){try{e.volume=x;e.defaultMuted=x<=0;e.muted=x<=0?true:false;}catch(_){}}};"
           << L"document.addEventListener('play',window.__homepanelStationheadVolumeApply,true);"
           << L"document.addEventListener('loadedmetadata',window.__homepanelStationheadVolumeApply,true);"
           << L"window.__homepanelStationheadVolumeTimer=setInterval(window.__homepanelStationheadVolumeApply,1000);"
           << L"} true; })()";
    view->ExecuteScript(script.str().c_str(), nullptr);
  };
  apply(webview_);
  apply(authWebview_);
}

void SecondaryStationheadPlayer::EnsureDistinctBrowserIdentity() noexcept {
  if (!webview_ || identityWebview_ == webview_.Get()) return;
  identityWebview_ = webview_.Get();
  ComPtr<ICoreWebView2Settings> settings;
  if (FAILED(webview_->get_Settings(&settings)) || !settings) return;
  ComPtr<ICoreWebView2Settings2> settings2;
  if (FAILED(settings.As(&settings2)) || !settings2) return;
  LPWSTR rawUserAgent = nullptr;
  if (FAILED(settings2->get_UserAgent(&rawUserAgent)) || !rawUserAgent) return;
  std::wstring userAgent(rawUserAgent);
  CoTaskMemFree(rawUserAgent);
  if (userAgent.find(L"HomePanelSecondary/") == std::wstring::npos) {
    userAgent += L" HomePanelSecondary/1.0";
    settings2->put_UserAgent(userAgent.c_str());
  }
}
}  // namespace hp
