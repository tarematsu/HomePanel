// Part of app.cpp's translation unit (see the #include at the end of that
// file). Win32 window procedure and the main message dispatch (timer, paint,
// input, and the WM_HP_* cross-thread notifications). Uses the WM_HP_UPDATE_RESULT
// constant and other file-local declarations from app.cpp.
#include "app.h"

namespace hp {

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

LRESULT App::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_TIMER:
      Tick();
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
    case WM_LBUTTONUP: {
      if (!renderer_) return 0;
      POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      float seek = 0;
      HandleAction(renderer_->HitTest(point, &seek), seek);
      return 0;
    }
    case WM_KEYDOWN:
      if (wParam == VK_F12) {
        renderState_.maintenance = !renderState_.maintenance;
        MarkRenderStateDirty();
        InvalidateAll();
      } else if (wParam == VK_ESCAPE && renderState_.maintenance) {
        renderState_.maintenance = false;
        MarkRenderStateDirty();
        InvalidateAll();
      }
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
        MarkRenderStateDirty();
      }

      const std::wstring monitorHandle = renderer_->MonitorHostHandle();
      if (!monitorHandle.empty() && stationhead_) {
        stationhead_->NotifyMonitorHandle(monitorHandle);
      }
      InvalidateAll();
      return 0;
    }
    case WM_HP_RADAR_UPDATED:
      if (renderer_) renderer_->NotifyRadarUpdated();
      return 0;
    case WM_HP_SWITCHBOT_UPDATED:
      sensors_->ApplyCloudSwitchBot(dataDir_ / L"switchbot.json");
      renderState_.sensors = sensors_->Snapshot();
      MarkRenderStateDirty();
      InvalidateAll();
      return 0;
    case WM_HP_SENSOR_UPDATED:
      renderState_.sensors = sensors_->Snapshot();
      UpdateAirHistory(renderState_.sensors);
      MarkRenderStateDirty();
      InvalidateAll();
      return 0;
    case WM_HP_STATIONHEAD_CHANGED: {
      const uint32_t changes = stationhead_->ConsumeChangeFlags();
      bool layoutChanged = false;
      if ((changes & StationheadChangeReleaseAuth) != 0) {
        stationhead_->ReleaseCompletedAuth();
      }
      if ((changes & StationheadChangeShowPlayer) != 0) {
        stationhead_->ShowAfterAudioStop();
        if (selectedTab_ != WorkspaceTab::Stationhead) {
          selectedTab_ = WorkspaceTab::Stationhead;
          layoutChanged = true;
        }
      } else if ((changes & StationheadChangeReturnMain) != 0 &&
                 selectedTab_ != WorkspaceTab::Main) {
        selectedTab_ = WorkspaceTab::Main;
        layoutChanged = true;
      }
      if ((changes & StationheadChangeScheduledReload) != 0) {
        ApplyScheduledStationheadAudioProfile(!scheduledPrimaryAudioAudible_);
      }
      if (layoutChanged) LayoutWorkspace();
      const StationheadStatus nextStationheadState =
          BuildRenderStationheadState(stationhead_, secondaryStationhead_);
      UpdateRenderStationheadState(nextStationheadState);
      Invalidate(renderer_->StationheadRect());
      return 0;
    }
    case WM_HP_CONFIG_UPDATED:
      renderState_.toast = L"クラウド設定を保存しました。再起動時に適用します";
      ShowToast(std::move(renderState_.toast), 5000);
      return 0;
    case WM_HP_COMMANDS_UPDATED:
      ProcessRemoteCommands();
      return 0;
    case WM_HP_UPDATE_RESULT: {
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
