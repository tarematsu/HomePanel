#include "app.h"
#include "web_renderer.h"

namespace hp {
namespace {
constexpr UINT kStationheadHealthUpdatedMessage = WM_APP + 10;
constexpr int64_t kStationheadHandoffAudioStabilityMs = 1'500;

constexpr int64_t StableAudioReadyAt(
    int64_t requestedReadyAtMs,
    int64_t audioPlayingSinceMs) noexcept {
  if (audioPlayingSinceMs <= 0) return requestedReadyAtMs;
  const int64_t continuousReadyAt =
      audioPlayingSinceMs + kStationheadHandoffAudioStabilityMs;
  return std::max(requestedReadyAtMs, continuousReadyAt);
}

enum class TrackBoundaryPendingAction {
  Wait,
  CancelExpired,
  CancelResumed,
  Retry,
};

constexpr TrackBoundaryPendingAction TrackBoundaryPendingActionFor(
    int64_t nowMs,
    int64_t pendingUntilMs,
    int64_t handoffReadyAtMs,
    bool pendingWindowPlaying,
    bool otherWindowRequired,
    bool otherWindowPlaying) noexcept {
  if (pendingUntilMs <= 0) return TrackBoundaryPendingAction::Wait;
  if (nowMs >= pendingUntilMs) return TrackBoundaryPendingAction::CancelExpired;
  // The ended message can beat WebView2's audio-stopped notification. Do not
  // call a still-true flag "next track resumed" until the initial stability
  // window has elapsed.
  if (nowMs < handoffReadyAtMs) return TrackBoundaryPendingAction::Wait;
  if (pendingWindowPlaying) return TrackBoundaryPendingAction::CancelResumed;
  if (otherWindowRequired && !otherWindowPlaying) {
    return TrackBoundaryPendingAction::Wait;
  }
  return TrackBoundaryPendingAction::Retry;
}

static_assert(kStationheadHandoffAudioStabilityMs >= 1'000);
static_assert(StableAudioReadyAt(150, 0) == 150);
static_assert(StableAudioReadyAt(150, 100) == 1'600);
static_assert(StableAudioReadyAt(2'000, 100) == 2'000);
static_assert(TrackBoundaryPendingActionFor(100, 0, 0, false, true, true) ==
              TrackBoundaryPendingAction::Wait);
static_assert(TrackBoundaryPendingActionFor(100, 200, 150, true, true, true) ==
              TrackBoundaryPendingAction::Wait);
static_assert(TrackBoundaryPendingActionFor(160, 200, 150, true, true, true) ==
              TrackBoundaryPendingAction::CancelResumed);
static_assert(TrackBoundaryPendingActionFor(200, 200, 150, false, true, true) ==
              TrackBoundaryPendingAction::CancelExpired);
static_assert(TrackBoundaryPendingActionFor(100, 200, 150, false, true, true) ==
              TrackBoundaryPendingAction::Wait);
static_assert(TrackBoundaryPendingActionFor(160, 200, 150, false, true, false) ==
              TrackBoundaryPendingAction::Wait);
static_assert(TrackBoundaryPendingActionFor(160, 200, 150, false, true, true) ==
              TrackBoundaryPendingAction::Retry);
static_assert(TrackBoundaryPendingActionFor(160, 200, 150, false, false, false) ==
              TrackBoundaryPendingAction::Retry);
}

LRESULT CALLBACK App::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
    if (app) app->window_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
  }
  return app ? app->HandleMessage(message, wParam, lParam)
             : DefWindowProcW(window, message, wParam, lParam);
}

void App::ProcessPendingStationheadTrackBoundaryRefreshes(int64_t nowMs) {
  const auto process = [this, nowMs](
                           int64_t& pendingUntil,
                           int64_t& handoffReadyAt,
                           auto& pendingPlayer,
                           auto& otherPlayer,
                           bool otherRequired,
                           const wchar_t* roleTag) {
    if (pendingUntil <= 0) return;
    const bool pendingPlaying = pendingPlayer && pendingPlayer->AudioPlaying();
    const bool otherPlaying =
        !otherRequired || (otherPlayer && otherPlayer->AudioPlaying());
    const int64_t otherPlayingSince =
        otherRequired && otherPlayer ? otherPlayer->AudioPlayingSince() : nowMs;
    if (otherRequired) {
      if (!otherPlaying || otherPlayingSince <= 0) {
        // Require a fresh, uninterrupted stability window after every observed
        // loss of the handoff side's audio. A one-frame WebView2 audio pulse must
        // not authorize navigation of the other player.
        handoffReadyAt = nowMs + kStationheadHandoffAudioStabilityMs;
      } else {
        // The per-player timestamp catches a stop/start transition even when
        // coalesced window messages hide the intermediate stopped state.
        handoffReadyAt = StableAudioReadyAt(
            handoffReadyAt, otherPlayingSince);
      }
    }
    const TrackBoundaryPendingAction action = TrackBoundaryPendingActionFor(
        nowMs, pendingUntil, handoffReadyAt, pendingPlaying,
        otherRequired, otherPlaying);
    switch (action) {
      case TrackBoundaryPendingAction::CancelExpired:
        pendingUntil = 0;
        handoffReadyAt = 0;
        if (pendingPlayer) pendingPlayer->CancelPendingTrackBoundaryRefresh();
        if (logger_) {
          logger_->Warn(L"Stationhead " + std::wstring(roleTag) +
                        L" pending track-boundary refresh expired");
        }
        return;
      case TrackBoundaryPendingAction::CancelResumed:
        pendingUntil = 0;
        handoffReadyAt = 0;
        if (pendingPlayer) pendingPlayer->CancelPendingTrackBoundaryRefresh();
        if (logger_) {
          logger_->Info(L"Stationhead " + std::wstring(roleTag) +
                        L" pending track-boundary refresh cancelled because the next track already started");
        }
        return;
      case TrackBoundaryPendingAction::Retry:
        if (!pendingPlayer) {
          pendingUntil = 0;
          handoffReadyAt = 0;
          return;
        }
        // Keep the deadline armed while RetryPendingTrackBoundaryRefresh sends
        // the synchronous readiness message. The message handler clears it only
        // after re-checking both players at the actual navigation instant.
        pendingPlayer->RetryPendingTrackBoundaryRefresh(nowMs);
        return;
      case TrackBoundaryPendingAction::Wait:
      default:
        return;
    }
  };

  process(primaryTrackBoundaryPendingUntil_, primaryTrackBoundaryHandoffReadyAt_,
          stationhead_, secondaryStationhead_,
          static_cast<bool>(secondaryStationhead_), L"A");
  process(secondaryTrackBoundaryPendingUntil_, secondaryTrackBoundaryHandoffReadyAt_,
          secondaryStationhead_, stationhead_, true, L"B");
}

LRESULT App::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_TIMER:
      Tick();
      ProcessPendingStationheadTrackBoundaryRefreshes(UnixMillis());
      if (cloud_ && toastUntil_ == 0 && renderState_.toast.empty()) {
        std::wstring health = cloud_->StationheadHealthText();
        if (renderState_.toast != health) {
          renderState_.toast = std::move(health);
          PublishRenderStateNow();
        }
      }
      return 0;
    case WM_PAINT:
      Draw();
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      if (renderer_ && wParam != SIZE_MINIMIZED) {
        renderer_->Resize(LOWORD(lParam), HIWORD(lParam));
        LayoutWorkspace();
      }
      return 0;
    case WM_LBUTTONUP:
      if (renderer_) HandleAction(renderer_->TakePendingAction());
      return 0;
    case kRendererActionMessage:
      HandleAction(static_cast<UiAction>(wParam));
      return 0;
    case WM_HP_CLOUD_UPDATED: {
      bool dashboardChanged = false;
      if (!renderer_->LoadDashboard(dataDir_ / L"dashboard.json", &dashboardChanged) ||
          !dashboardChanged) {
        return 0;
      }

      renderState_.toast = L"表示データを更新しました";
      const int64_t now = UnixMillis();
      ShowToast(std::move(renderState_.toast), 4000, false);

      const int count = renderer_->NewsCount();
      if (count != newsCount_) {
        newsCount_ = count;
        newsIndex_ = 0;
        lastNewsRotateAt_ = count > 1 ? now : 0;
        renderState_.newsIndex = 0;
      }

      PublishRenderStateNow();
      return 0;
    }
    case WM_HP_RADAR_UPDATED:
      if (renderer_) renderer_->NotifyRadarUpdated();
      return 0;
    case WM_HP_SWITCHBOT_UPDATED:
      sensors_->ApplyCloudSwitchBot(dataDir_ / L"switchbot.json");
      renderState_.sensors = sensors_->Snapshot();
      PublishRenderStateNow();
      return 0;
    case WM_HP_SENSOR_UPDATED:
      renderState_.sensors = sensors_->Snapshot();
      UpdateAirHistory(renderState_.sensors);
      PublishRenderStateNow();
      return 0;
    case WM_HP_PRIMARY_RELOAD_READY: {
      const int64_t now = UnixMillis();
      if (primaryTrackBoundaryPendingUntil_ <= 0) {
        primaryTrackBoundaryPendingUntil_ =
            now + kStationheadTrackTransitionGraceMs;
        primaryTrackBoundaryHandoffReadyAt_ =
            now + kStationheadHandoffAudioStabilityMs;
        return 0;
      }
      if (stationhead_->AudioPlaying()) {
        primaryTrackBoundaryPendingUntil_ = 0;
        primaryTrackBoundaryHandoffReadyAt_ = 0;
        stationhead_->CancelPendingTrackBoundaryRefresh();
        if (logger_) {
          logger_->Info(
              L"Stationhead A track-boundary refresh cancelled at readiness check because the next track already started");
        }
        return 0;
      }
      if (now >= primaryTrackBoundaryPendingUntil_) {
        primaryTrackBoundaryPendingUntil_ = 0;
        primaryTrackBoundaryHandoffReadyAt_ = 0;
        stationhead_->CancelPendingTrackBoundaryRefresh();
        return 0;
      }
      if (secondaryStationhead_) {
        const bool handoffPlaying = secondaryStationhead_->AudioPlaying();
        const int64_t handoffPlayingSince =
            secondaryStationhead_->AudioPlayingSince();
        if (!handoffPlaying || handoffPlayingSince <= 0) {
          primaryTrackBoundaryHandoffReadyAt_ =
              now + kStationheadHandoffAudioStabilityMs;
          return 0;
        }
        primaryTrackBoundaryHandoffReadyAt_ = StableAudioReadyAt(
            primaryTrackBoundaryHandoffReadyAt_, handoffPlayingSince);
      }
      if (now < primaryTrackBoundaryHandoffReadyAt_) return 0;
      primaryTrackBoundaryPendingUntil_ = 0;
      primaryTrackBoundaryHandoffReadyAt_ = 0;
      if (secondaryStationhead_) ApplyScheduledStationheadAudioProfile(false);
      return 1;
    }
    case WM_HP_SECONDARY_RELOAD_READY: {
      if (!secondaryStationhead_) return 0;
      const int64_t now = UnixMillis();
      if (secondaryTrackBoundaryPendingUntil_ <= 0) {
        secondaryTrackBoundaryPendingUntil_ =
            now + kStationheadTrackTransitionGraceMs;
        secondaryTrackBoundaryHandoffReadyAt_ =
            now + kStationheadHandoffAudioStabilityMs;
        return 0;
      }
      if (secondaryStationhead_->AudioPlaying()) {
        secondaryTrackBoundaryPendingUntil_ = 0;
        secondaryTrackBoundaryHandoffReadyAt_ = 0;
        secondaryStationhead_->CancelPendingTrackBoundaryRefresh();
        if (logger_) {
          logger_->Info(
              L"Stationhead B track-boundary refresh cancelled at readiness check because the next track already started");
        }
        return 0;
      }
      if (now >= secondaryTrackBoundaryPendingUntil_) {
        secondaryTrackBoundaryPendingUntil_ = 0;
        secondaryTrackBoundaryHandoffReadyAt_ = 0;
        secondaryStationhead_->CancelPendingTrackBoundaryRefresh();
        return 0;
      }
      const bool handoffPlaying = stationhead_->AudioPlaying();
      const int64_t handoffPlayingSince = stationhead_->AudioPlayingSince();
      if (!handoffPlaying || handoffPlayingSince <= 0) {
        secondaryTrackBoundaryHandoffReadyAt_ =
            now + kStationheadHandoffAudioStabilityMs;
        return 0;
      }
      secondaryTrackBoundaryHandoffReadyAt_ = StableAudioReadyAt(
          secondaryTrackBoundaryHandoffReadyAt_, handoffPlayingSince);
      if (now < secondaryTrackBoundaryHandoffReadyAt_) return 0;
      secondaryTrackBoundaryPendingUntil_ = 0;
      secondaryTrackBoundaryHandoffReadyAt_ = 0;
      ApplyScheduledStationheadAudioProfile(true);
      return 1;
    }
    case WM_HP_STATIONHEAD_CHANGED: {
      const uint32_t primaryChanges = stationhead_->ConsumeChangeFlags();
      const uint32_t secondaryChanges = secondaryStationhead_
          ? secondaryStationhead_->ConsumeChangeFlags()
          : 0;
      const uint32_t changes = primaryChanges | secondaryChanges;
      const int64_t now = UnixMillis();
      ProcessPendingStationheadTrackBoundaryRefreshes(now);
      bool layoutChanged = false;
      if ((primaryChanges & StationheadChangeReleaseAuth) != 0) {
        stationhead_->ReleaseCompletedAuth();
      }
      if (secondaryStationhead_ &&
          (secondaryChanges & StationheadChangeReleaseAuth) != 0) {
        secondaryStationhead_->ReleaseCompletedAuth();
      }
      bool showPlayer = false;
      if ((primaryChanges & StationheadChangeShowPlayer) != 0) {
        stationhead_->ShowAfterAudioStop();
        showPlayer = true;
      }
      if (secondaryStationhead_ &&
          (secondaryChanges & StationheadChangeShowPlayer) != 0) {
        secondaryStationhead_->ShowAfterAudioStop();
        showPlayer = true;
      }
      if (showPlayer) {
        if (selectedTab_ != WorkspaceTab::Stationhead) {
          selectedTab_ = WorkspaceTab::Stationhead;
          layoutChanged = true;
        }
      } else if ((changes & StationheadChangeReturnMain) != 0 &&
                 selectedTab_ != WorkspaceTab::Main) {
        selectedTab_ = WorkspaceTab::Main;
        layoutChanged = true;
      }
      if (layoutChanged) LayoutWorkspace();
      MarkStationheadPlacementDirty();
      StationheadStatus renderStationheadState = stationhead_->Status();
      StationheadStatus secondaryStatus =
          secondaryStationhead_ ? secondaryStationhead_->Status() : StationheadStatus{};
      EnrichRenderStationheadState(
          renderStationheadState,
          secondaryStationhead_ ? &secondaryStatus : nullptr,
          config_.stationhead);
      renderStationheadState.primaryAudioSelected = scheduledPrimaryAudioAudible_;
      const bool stateChanged =
          UpdateRenderStationheadState(std::move(renderStationheadState));
      if (!rendererStarted_) {
        StartDeferredServices(UnixMillis(), renderState_.stationhead);
      } else if (!layoutChanged && stateChanged) {
        if (selectedTab_ == WorkspaceTab::Main) {
          ApplyStationheadWindowPlacement(renderState_.stationhead, secondaryStatus);
        } else {
          LayoutWorkspace();
        }
      }
      if (layoutChanged || stateChanged) PublishRenderStateNow();
      return 0;
    }
    case kStationheadHealthUpdatedMessage:
      if (cloud_ && toastUntil_ == 0) {
        std::wstring health = cloud_->StationheadHealthText();
        if (renderState_.toast != health) {
          renderState_.toast = std::move(health);
          PublishRenderStateNow();
        }
      }
      return 0;
    case WM_HP_CONFIG_UPDATED:
      renderState_.toast = L"クラウド設定を保存しました。再起動時に適用します";
      ShowToast(std::move(renderState_.toast), 5000);
      return 0;
    case WM_HP_COMMANDS_UPDATED:
      ProcessRemoteCommands();
      return 0;
    case kUpdateResultMessage: {
      std::unique_ptr<wchar_t[]> updateMessage(reinterpret_cast<wchar_t*>(lParam));
      if (updateMessage && updateMessage[0] != L'\0') {
        renderState_.toast = updateMessage.get();
        ShowToast(updateMessage.get(), 7000);
      }
      return 0;
    }
    case WM_CLOSE:
      DestroyWindow(window_);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(exitCode_);
      return 0;
  }
  return DefWindowProcW(window_, message, wParam, lParam);
}

}  // namespace hp
