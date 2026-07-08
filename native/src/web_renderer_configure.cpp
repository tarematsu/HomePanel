#include "web_renderer.h"
#include "app.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {
void Renderer::ConfigureWebView() {
  if (!controller_ || !webview_) {
    SetWebViewError(L"ConfigureWebView input", E_POINTER);
    return;
  }
  Resize(width_, height_);

  ComPtr<ICoreWebView2Controller2> controller2;
  if (SUCCEEDED(controller_.As(&controller2))) {
    COREWEBVIEW2_COLOR opaqueBackground{255, 5, 8, 13};
    const HRESULT backgroundResult = controller2->put_DefaultBackgroundColor(opaqueBackground);
    if (FAILED(backgroundResult)) AppendWebViewDiagnostic(L"Default background color failed");
  }

  ComPtr<ICoreWebView2Settings> settings;
  webview_->get_Settings(&settings);
  if (settings) {
    settings->put_AreDefaultContextMenusEnabled(FALSE);
    settings->put_AreDevToolsEnabled(FALSE);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_IsZoomControlEnabled(FALSE);
  }

  ComPtr<ICoreWebView2_3> webview3;
  if (FAILED(webview_.As(&webview3)) || !webview3) {
    SetWebViewError(L"ICoreWebView2_3", E_NOINTERFACE);
    return;
  }
  const HRESULT appMappingResult = webview3->SetVirtualHostNameToFolderMapping(
      L"app.homepanel", uiDir_.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
  if (FAILED(appMappingResult)) {
    SetWebViewError(L"Dashboard resource mapping", appMappingResult);
    return;
  }
  const HRESULT dataMappingResult = webview3->SetVirtualHostNameToFolderMapping(
      L"data.homepanel", dataDir_.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
  if (FAILED(dataMappingResult)) {
    SetWebViewError(L"Dashboard data mapping", dataMappingResult);
    return;
  }

  webview_->add_NavigationStarting(
      Callback<ICoreWebView2NavigationStartingEventHandler>(
          [this](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*) -> HRESULT {
            ready_ = false;
            uiReady_ = false;
            postedState_.clear();
            return S_OK;
          }).Get(),
      &navigationStartingToken_);

  webview_->add_NavigationCompleted(
      Callback<ICoreWebView2NavigationCompletedEventHandler>(
          [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            BOOL success = FALSE;
            args->get_IsSuccess(&success);
            ready_ = success == TRUE;
            if (!ready_) {
              uiReady_ = false;
              COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
              args->get_WebErrorStatus(&status);
              SetWebViewError(L"Dashboard navigation status " + std::to_wstring(static_cast<int>(status)), E_FAIL);
              return S_OK;
            }
            webViewError_.clear();
            AppendWebViewDiagnostic(L"Dashboard navigation completed");
            if (uiReady_) {
              postedState_.clear();
              PostFullState();
            }
            return S_OK;
          }).Get(),
      &navigationToken_);

  webview_->add_WebMessageReceived(
      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            LPWSTR raw = nullptr;
            if (FAILED(args->get_WebMessageAsJson(&raw)) || !raw) return S_OK;
            const std::wstring json = raw;
            CoTaskMemFree(raw);
            try {
              const auto object = winrt::Windows::Data::Json::JsonObject::Parse(json);
              const std::wstring type = object.GetNamedString(L"type", L"").c_str();
              if (type == L"ready") {
                if (uiReady_) return S_OK;
                uiReady_ = true;
                if (ready_) {
                  postedState_.clear();
                  PostFullState();
                  if (radarUpdatePending_) NotifyRadarUpdated();
                }
                return S_OK;
              }
              if (type != L"action") return S_OK;
              const std::wstring action = object.GetNamedString(L"action", L"").c_str();
              if (action == L"stationhead-audio-a") {
                if (App* app = App::Current()) app->ToggleStationheadAudioA();
                return S_OK;
              }
              if (action == L"stationhead-audio-b") {
                if (App* app = App::Current()) app->ToggleStationheadAudioB();
                return S_OK;
              }
              if (action == L"stationhead-volume-a" || action == L"stationhead-volume-b") {
                const double normalized = std::clamp(object.GetNamedNumber(L"value", 100.0), 0.0, 100.0) / 100.0;
                if (App* app = App::Current()) {
                  if (action == L"stationhead-volume-a") app->SetStationheadVolumeA(normalized);
                  else app->SetStationheadVolumeB(normalized);
                }
                return S_OK;
              }
              static const std::unordered_map<std::wstring, UiAction> actions = {
                  {L"app-update", UiAction::AppUpdate},
                  {L"restart", UiAction::Restart},
                  {L"radar-toggle", UiAction::RadarToggle},
                  {L"radar-previous", UiAction::RadarPrevious},
                  {L"radar-next", UiAction::RadarNext},
                  {L"radar-seek", UiAction::RadarSeek},
              };
              if (const auto found = actions.find(action); found != actions.end()) {
                const float value = static_cast<float>(object.GetNamedNumber(L"value", 0));
                QueueAction(found->second, value);
              }
            } catch (...) {
            }
            return S_OK;
          }).Get(),
      &messageToken_);

  webview_->add_ProcessFailed(
      Callback<ICoreWebView2ProcessFailedEventHandler>(
          [this](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
            ready_ = false;
            uiReady_ = false;
            postedState_.clear();
            COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
            if (args) args->get_ProcessFailedKind(&kind);
            SetWebViewError(L"WebView2 process kind " + std::to_wstring(static_cast<int>(kind)), E_FAIL);
            return S_OK;
          }).Get(),
      &processFailedToken_);

  const HRESULT visibleResult = controller_->put_IsVisible(TRUE);
  if (FAILED(visibleResult)) {
    SetWebViewError(L"Show WebView2 controller", visibleResult);
    return;
  }
  controllerVisible_ = true;
  ApplyDashboardHostBounds();
  // The dashboard controller is created after Stationhead startup and can become the
  // newest child window. Re-run the normal workspace layout so an active login or
  // recovery surface is restored above it without inventing a second Z-order system.
  PostMessageW(window_, WM_SIZE, SIZE_RESTORED,
               MAKELPARAM(static_cast<WORD>(width_), static_cast<WORD>(height_)));
  ready_ = false;
  uiReady_ = false;
  postedState_.clear();
  const HRESULT navigateResult = webview_->Navigate(L"https://app.homepanel/index.html");
  if (FAILED(navigateResult)) SetWebViewError(L"Start dashboard navigation", navigateResult);
}
}  // namespace hp
