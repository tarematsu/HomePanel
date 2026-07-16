#include "sh.h"
#include "sh_shared.h"

namespace hp {

void StationheadPlayer::SetMuted(bool muted) noexcept {
  audioMuted_.store(muted, std::memory_order_relaxed);
  ApplyMute();
}

bool StationheadPlayer::Muted() const noexcept {
  return audioMuted_.load(std::memory_order_relaxed);
}

void StationheadPlayer::SetVolume(double volume) noexcept {
  audioVolume_.store(std::clamp(volume, 0.0, 1.0), std::memory_order_relaxed);
  ApplyVolume();
}

double StationheadPlayer::Volume() const noexcept {
  return audioVolume_.load(std::memory_order_relaxed);
}

void StationheadPlayer::ApplyMute() const noexcept {
  const int muted = audioMuted_.load(std::memory_order_relaxed) ? 1 : 0;
  if (appliedMuted_.exchange(muted, std::memory_order_relaxed) != muted) {
    const BOOL value = muted ? TRUE : FALSE;
    const auto apply = [value](const ComPtr<ICoreWebView2>& view) noexcept {
      if (!view) return;
      ComPtr<ICoreWebView2_8> audio;
      if (SUCCEEDED(view.As(&audio)) && audio) audio->put_IsMuted(value);
    };
    apply(webview_);
    apply(authWebview_);
  }
  ApplyVolume();
}

void StationheadPlayer::ApplyVolume() const noexcept {
  const int percent = std::clamp(
      static_cast<int>(audioVolume_.load(std::memory_order_relaxed) * 100.0 + 0.5), 0, 100);
  if (appliedVolumePercent_.exchange(percent, std::memory_order_relaxed) == percent) return;
  const auto apply = [percent](const ComPtr<ICoreWebView2>& view) noexcept {
    if (!view) return;
    const std::wstring script = StationheadVolumeScript(percent);
    view->ExecuteScript(script.c_str(), nullptr);
  };
  apply(webview_);
  apply(authWebview_);
}

// Window B's isolated WebView2 environment still ships the platform's default
// user agent, which is otherwise identical to Window A's. Tag it distinctly
// so Stationhead's own session/device bookkeeping does not conflate the two
// independent, cookie-isolated sessions with a single device identity.
void StationheadPlayer::EnsureDistinctBrowserIdentity() noexcept {
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
