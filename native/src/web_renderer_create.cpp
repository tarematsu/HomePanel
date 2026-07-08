#include "web_renderer.h"
#include "shared_webview_environment.h"

namespace hp {
void Renderer::CreateWebView() {
  if (creating_.exchange(true) || shuttingDown_) return;
  webViewStartedAt_ = UnixMillis();
  webViewError_.clear();

  runtimeVersion_.clear();
  AppendWebViewDiagnostic(L"WebView2 startup requested");

  if (!EnsureDashboardHostWindow()) {
    creating_ = false;
    SetWebViewError(L"Create dashboard host window", E_FAIL);
    return;
  }

  SharedWebViewEnvironment::Instance().Acquire(
      userDataDir_,
      [this](HRESULT result, ICoreWebView2Environment* environment) {
        if (shuttingDown_) {
          creating_ = false;
          return;
        }
        if (FAILED(result) || !environment) {
          SetWebViewError(L"Acquire shared WebView2 environment", FAILED(result) ? result : E_POINTER);
          return;
        }

        environment_ = environment;
        AppendWebViewDiagnostic(L"Shared WebView2 environment acquired");
        if (!EnsureDashboardHostWindow()) {
          creating_ = false;
          SetWebViewError(L"Dashboard host window unavailable", E_FAIL);
          return;
        }
        const HRESULT controllerStarted = environment_->CreateCoreWebView2Controller(
            dashboardHost_, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                          [this](HRESULT controllerResult, ICoreWebView2Controller* controller) -> HRESULT {
                            creating_ = false;
                            if (shuttingDown_) {
                              if (controller) controller->Close();
                              return S_OK;
                            }
                            if (FAILED(controllerResult) || !controller) {
                              SetWebViewError(L"CreateCoreWebView2Controller",
                                              FAILED(controllerResult) ? controllerResult : E_POINTER);
                              return S_OK;
                            }
                            controller_ = controller;
                            controllerBoundsValid_ = false;
                            controllerVisible_ = false;
                            const HRESULT webViewResult = controller_->get_CoreWebView2(&webview_);
                            if (FAILED(webViewResult) || !webview_) {
                              SetWebViewError(L"get_CoreWebView2",
                                              FAILED(webViewResult) ? webViewResult : E_POINTER);
                              return S_OK;
                            }
                            AppendWebViewDiagnostic(L"WebView2 controller created");
                            ConfigureWebView();
                            return S_OK;
                          }).Get());
        if (FAILED(controllerStarted)) {
          SetWebViewError(L"Start CreateCoreWebView2Controller", controllerStarted);
        }
      });
}

void Renderer::CloseWebView() {
  ready_ = false;
  uiReady_ = false;
  creating_ = false;
  controllerVisible_ = false;
  controllerBoundsValid_ = false;
  appliedControllerBounds_ = RECT{};
  postedState_.clear();
  if (webview_) {
    webview_->remove_NavigationStarting(navigationStartingToken_);
    webview_->remove_NavigationCompleted(navigationToken_);
    webview_->remove_WebMessageReceived(messageToken_);
    webview_->remove_ProcessFailed(processFailedToken_);
  }
  if (controller_) controller_->Close();
  webview_.Reset();
  controller_.Reset();
  environment_.Reset();
  DestroyDashboardHostWindow();
}
}  // namespace hp
