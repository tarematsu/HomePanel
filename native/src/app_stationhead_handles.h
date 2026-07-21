#pragma once
#include "sh.h"

namespace hp {

inline constexpr int64_t kStationheadTrackTransitionGraceMs = 12'000;

inline bool StationheadNeedsForeground(const StationheadStatus& status) noexcept {
  return !status.audioPlaying;
}

enum class WorkspaceTab {
  Main = 0,
  Stationhead = 1,
  Auth = 2,
};

class StationheadHandleBase {
 public:
  StationheadHandleBase(const StationheadHandleBase&) = delete;
  StationheadHandleBase& operator=(const StationheadHandleBase&) = delete;

  explicit operator bool() const noexcept;
  [[nodiscard]] bool AudioPlaying() const noexcept {
    return player_ && player_->AudioPlaying();
  }
  void Stop();
  void SetAudioMuted(bool muted) noexcept;
  void SetBounds(const RECT& bounds);
  void SetStartupPreviewBounds(const RECT& bounds);
  void ClearStartupPreviewBounds();
  StationheadStatus RawStatus() const;
  StationheadStatus Status() const;
  int64_t NextWakeAt() const noexcept;
  void RefreshVisibility();
  void Start();
  void Tick(int64_t nowMs);
  void Reconnect();
  void RetryPendingTrackBoundaryRefresh(int64_t nowMs);
  void CancelPendingTrackBoundaryRefresh() noexcept;
  void SetPlaybackFallback(bool active, const std::wstring& reason);
  void ShowAfterAudioStop();
  void ReleaseCompletedAuth();
  uint32_t ConsumeChangeFlags();

 protected:
  StationheadHandleBase() = default;
  ~StationheadHandleBase() = default;

  void AssignPlayer(std::unique_ptr<StationheadPlayer> player) noexcept;
  void ResetPlayer() noexcept;
  bool HasAuthTabPlayer() const;
  void SelectPlayerTab(StationheadTabKind tab);

 private:
  bool IsInteractive(const StationheadStatus& status) const noexcept;
  bool SuppressTrackTransitionGap(bool playing, bool forceInteractive) const noexcept;
  void ApplyAudioState() const noexcept;
  void BringMainWindowToFront(HWND host) const noexcept;
  void RaiseActiveHost() const;
  void ApplyInteractiveBounds();
  void ApplyBounds();

  std::unique_ptr<StationheadPlayer> player_;
  RECT workspaceBounds_{0, 0, 1, 1};
  RECT startupPreviewBounds_{0, 0, 1, 1};
  bool startupPreviewActive_ = false;
  bool audioMuted_ = false;
  mutable bool playbackObserved_ = false;
  mutable int64_t playbackMissingSinceAt_ = 0;
  mutable bool transitionSuppressed_ = false;
  mutable uint64_t contentRevision_ = 1;
};

class AppStationheadHandle final : public StationheadHandleBase {
 public:
  AppStationheadHandle() = default;
  AppStationheadHandle(const AppStationheadHandle&) = delete;
  AppStationheadHandle& operator=(const AppStationheadHandle&) = delete;

  AppStationheadHandle* operator->() noexcept;
  const AppStationheadHandle* operator->() const noexcept;
  AppStationheadHandle& operator=(std::unique_ptr<StationheadPlayer> player) noexcept;
  void reset() noexcept;
  bool HasAuthTab() const;
  void SelectTab(StationheadTabKind tab);
};

class AppSecondaryStationheadHandle final : public StationheadHandleBase {
 public:
  AppSecondaryStationheadHandle() = default;
  AppSecondaryStationheadHandle(const AppSecondaryStationheadHandle&) = delete;
  AppSecondaryStationheadHandle& operator=(const AppSecondaryStationheadHandle&) = delete;

  AppSecondaryStationheadHandle* operator->() noexcept;
  const AppSecondaryStationheadHandle* operator->() const noexcept;
  AppSecondaryStationheadHandle& operator=(
      std::unique_ptr<StationheadPlayer> player) noexcept;
  void reset() noexcept;
};

}  // namespace hp
