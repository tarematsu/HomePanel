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
  const BOOL muted = audioMuted_.load(std::memory_order_relaxed) ? TRUE : FALSE;
  const auto apply = [muted](const ComPtr<ICoreWebView2>& view) noexcept {
    if (!view) return;
    ComPtr<ICoreWebView2_8> audio;
    if (SUCCEEDED(view.As(&audio)) && audio) audio->put_IsMuted(muted);
  };
  apply(webview_);
  apply(authWebview_);
  ApplyVolume();
}

void StationheadPlayer::ApplyVolume() const noexcept {
  const int percent = std::clamp(static_cast<int>(audioVolume_.load(std::memory_order_relaxed) * 100.0 + 0.5), 0, 100);
  const auto apply = [percent](const ComPtr<ICoreWebView2>& view) noexcept {
    if (!view) return;
    const std::wstring script = StationheadVolumeScript(percent);
    view->ExecuteScript(script.c_str(), nullptr);
  };
  apply(webview_);
  apply(authWebview_);
}
}  // namespace hp
