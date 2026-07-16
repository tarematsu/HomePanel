#include "app_stationhead_handles.h"

namespace hp {
namespace {
constexpr int64_t kTrackTransitionGraceMs = 12'000;

bool RequiresInteractiveStationhead(const StationheadStatus& status) noexcept {
  return status.loginRequired || status.spotifyAuthorization || status.processFailed;
}
}  // namespace

StationheadHandleBase::operator bool() const noexcept {
  return static_cast<bool>(player_);
}

void StationheadHandleBase::Stop() {
  if (player_) player_->Stop();
}

void StationheadHandleBase::SetAudioMuted(bool muted) noexcept {
  if (audioMuted_ == muted) return;
  audioMuted_ = muted;
  if (player_) player_->SetMuted(muted);
}

void StationheadHandleBase::SetBounds(const RECT& bounds) {
  if (!startupPreviewActive_ && EqualRect(&workspaceBounds_, &bounds)) return;
  workspaceBounds_ = bounds;
  ApplyBounds();
}

void StationheadHandleBase::SetStartupPreviewBounds(const RECT& bounds) {
  startupPreviewBounds_ = bounds;
  startupPreviewActive_ = true;
  ApplyBounds();
}

void StationheadHandleBase::ClearStartupPreviewBounds() {
  if (!startupPreviewActive_) return;
  startupPreviewActive_ = false;
  ApplyBounds();
}

StationheadStatus StationheadHandleBase::RawStatus() const {
  StationheadStatus status = player_ ? player_->Status() : StationheadStatus{};
  status.audioMuted = audioMuted_;
  return status;
}

StationheadStatus StationheadHandleBase::Status() const {
  StationheadStatus status = RawStatus();
  if (player_ && SuppressTrackTransitionGap(
                     status.audioPlaying, RequiresInteractiveStationhead(status))) {
    if (status.visible) player_->KeepPlaybackBehindDashboard();
    status.audioPlaying = true;
    status.playing = true;
    status.visible = false;
    status.detail = L"track transition; waiting for next audio";
  }
  return status;
}

int64_t StationheadHandleBase::NextWakeAt() const noexcept {
  return player_ ? player_->NextWakeAt() : 0;
}

void StationheadHandleBase::RefreshVisibility() {
  if (!player_) return;
  const StationheadStatus status = player_->Status();
  if (SuppressTrackTransitionGap(
          status.audioPlaying, RequiresInteractiveStationhead(status))) {
    if (status.visible) player_->KeepPlaybackBehindDashboard();
    return;
  }
  player_->SelectTab(StationheadTabKind::None);
  ApplyBounds();
}

void StationheadHandleBase::Start() {
  if (!player_) return;
  ApplyInteractiveBounds();
  player_->Start();
  ApplyAudioState();
  ApplyBounds();
}

void StationheadHandleBase::Tick(int64_t nowMs) {
  if (player_) player_->Tick(nowMs);
}

void StationheadHandleBase::Reconnect() {
  if (!player_) return;
  ApplyInteractiveBounds();
  player_->Reconnect();
  ApplyBounds();
}

void StationheadHandleBase::SetPlaybackFallback(
    bool active, const std::wstring& reason) {
  if (!player_) return;
  player_->SetPlaybackFallback(active, reason);
  ApplyBounds();
}

void StationheadHandleBase::ShowAfterAudioStop() {
  if (!player_) return;
  ApplyInteractiveBounds();
  player_->ShowAfterAudioStop();
  ApplyBounds();
}

void StationheadHandleBase::ReleaseCompletedAuth() {
  if (!player_) return;
  player_->ReleaseCompletedAuth();
  ApplyBounds();
}

uint32_t StationheadHandleBase::ConsumeChangeFlags() {
  return player_ ? player_->ConsumeChangeFlags() : StationheadChangeNone;
}

void StationheadHandleBase::AssignPlayer(
    std::unique_ptr<StationheadPlayer> player) noexcept {
  player_ = std::move(player);
  ApplyAudioState();
  ApplyBounds();
}

void StationheadHandleBase::ResetPlayer() noexcept {
  player_.reset();
  playbackObserved_ = false;
  playbackMissingSinceAt_ = 0;
}

bool StationheadHandleBase::HasAuthTabPlayer() const {
  return player_ && player_->HasAuthTab();
}

void StationheadHandleBase::SelectPlayerTab(StationheadTabKind tab) {
  if (!player_) return;
  if (tab == StationheadTabKind::None) {
    RefreshVisibility();
    return;
  }
  ApplyInteractiveBounds();
  player_->SelectTab(tab);
  ApplyBounds();
}

bool StationheadHandleBase::IsInteractive(
    const StationheadStatus& status) const noexcept {
  if (RequiresInteractiveStationhead(status)) return true;
  return !status.audioPlaying &&
         !SuppressTrackTransitionGap(status.audioPlaying, false);
}

bool StationheadHandleBase::SuppressTrackTransitionGap(
    bool playing, bool forceInteractive) const noexcept {
  if (playing) {
    playbackObserved_ = true;
    playbackMissingSinceAt_ = 0;
    return false;
  }
  if (forceInteractive || !playbackObserved_) {
    playbackMissingSinceAt_ = 0;
    return false;
  }
  const int64_t now = UnixMillis();
  if (playbackMissingSinceAt_ == 0) playbackMissingSinceAt_ = now;
  return now - playbackMissingSinceAt_ < kTrackTransitionGraceMs;
}

void StationheadHandleBase::ApplyAudioState() const noexcept {
  if (player_) player_->SetMuted(audioMuted_);
}

void StationheadHandleBase::BringMainWindowToFront(HWND host) const noexcept {
  if (!host || !IsWindow(host)) return;
  HWND root = GetAncestor(host, GA_ROOT);
  if (!root || !IsWindow(root) || GetForegroundWindow() == root) return;
  SetWindowPos(root, HWND_TOP, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOSENDCHANGING);
  UpdateWindow(root);
}

void StationheadHandleBase::RaiseActiveHost() const {
  if (!player_) return;
  const bool preview = startupPreviewActive_;
  if (!preview && !player_->SurfaceVisible()) return;
  HWND host = player_->ActiveHostWindowForAccountSetup();
  if (!host || !IsWindow(host)) return;

  bool interactive = false;
  if (!preview) {
    const StationheadStatus status = player_->Status();
    interactive = IsInteractive(status);
    if (!interactive && !status.visible) return;
  }

  const RECT activeBounds = preview ? startupPreviewBounds_ : workspaceBounds_;
  const int width = std::max(1L, activeBounds.right - activeBounds.left);
  const int height = std::max(1L, activeBounds.bottom - activeBounds.top);
  SetWindowPos(host, HWND_TOP, activeBounds.left, activeBounds.top, width, height,
               SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOSENDCHANGING);
  if (!preview && interactive) BringMainWindowToFront(host);
}

void StationheadHandleBase::ApplyInteractiveBounds() {
  if (!player_) return;
  player_->ClearStartupPreviewBounds();
  player_->SetBounds(workspaceBounds_);
}

void StationheadHandleBase::ApplyBounds() {
  if (!player_) return;
  if (startupPreviewActive_) {
    player_->SetStartupPreviewBounds(startupPreviewBounds_);
  } else {
    player_->ClearStartupPreviewBounds();
    player_->SetBounds(workspaceBounds_);
  }
  RaiseActiveHost();
}

AppStationheadHandle* AppStationheadHandle::operator->() noexcept {
  return this;
}

const AppStationheadHandle* AppStationheadHandle::operator->() const noexcept {
  return this;
}

AppStationheadHandle& AppStationheadHandle::operator=(
    std::unique_ptr<StationheadPlayer> player) noexcept {
  AssignPlayer(std::move(player));
  return *this;
}

void AppStationheadHandle::reset() noexcept {
  ResetPlayer();
}

bool AppStationheadHandle::HasAuthTab() const {
  return HasAuthTabPlayer();
}

void AppStationheadHandle::SelectTab(StationheadTabKind tab) {
  SelectPlayerTab(tab);
}

AppSecondaryStationheadHandle* AppSecondaryStationheadHandle::operator->() noexcept {
  return this;
}

const AppSecondaryStationheadHandle*
AppSecondaryStationheadHandle::operator->() const noexcept {
  return this;
}

AppSecondaryStationheadHandle& AppSecondaryStationheadHandle::operator=(
    std::unique_ptr<StationheadPlayer> player) noexcept {
  AssignPlayer(std::move(player));
  return *this;
}

void AppSecondaryStationheadHandle::reset() noexcept {
  ResetPlayer();
}

}  // namespace hp
