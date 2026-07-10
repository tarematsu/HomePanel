#include "shared_webview_environment.h"
#include <WebView2EnvironmentOptions.h>

namespace hp {
namespace {









constexpr wchar_t kWebView2Arguments[] =
    L"--disable-domain-reliability "
    L"--disable-breakpad "
    L"--disable-extensions "
    L"--disable-sync "
    L"--metrics-recording-only "
    L"--autoplay-policy=no-user-gesture-required "


    L"--disable-backgrounding-occluded-windows "
    L"--disable-features=MediaRouter,Translate,OptimizationGuideModelDownloading,AutofillServerCommunication,HardwareSecureDecryption,HardwareSecureDecryptionExperiment,HardwareSecureDecryptionFallback";

void ApplyWebView2ProcessHints() noexcept {
  static std::once_flag once;
  std::call_once(once, [] {
    SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
                            kWebView2Arguments);
  });
}
}

SharedWebViewEnvironment& SharedWebViewEnvironment::Instance() {
  static SharedWebViewEnvironment instance;
  return instance;
}

std::wstring SharedWebViewEnvironment::NormalizePath(const fs::path& path) {
  std::error_code error;
  fs::path normalized = fs::absolute(path, error);
  if (error) normalized = path;
  normalized = normalized.lexically_normal();
  std::wstring key = normalized.wstring();
  std::transform(key.begin(), key.end(), key.begin(), towlower);
  return key;
}

void SharedWebViewEnvironment::Acquire(const fs::path& userDataFolder,
                                       Completion completion) {
  if (!completion) return;

  const std::wstring requestedKey = NormalizePath(userDataFolder);
  ComPtr<ICoreWebView2Environment> readyEnvironment;
  bool startCreation = false;
  fs::path folderForCreation;

  {
    std::lock_guard lock(mutex_);
    Entry& entry = entries_[requestedKey];
    if (entry.userDataFolder.empty()) entry.userDataFolder = userDataFolder;
    ++entry.acquireCount;
    if (entry.environment) {
      readyEnvironment = entry.environment;
    } else {
      entry.pending.push_back(std::move(completion));
      if (!entry.creating) {
        entry.creating = true;
        startCreation = true;
        folderForCreation = entry.userDataFolder;
      }
    }
  }

  if (readyEnvironment) {
    completion(S_OK, readyEnvironment.Get());
    return;
  }
  if (!startCreation) return;

  std::error_code directoryError;
  fs::create_directories(folderForCreation, directoryError);
  if (directoryError) {
    Complete(requestedKey, HRESULT_FROM_WIN32(directoryError.value()), nullptr);
    return;
  }





  ApplyWebView2ProcessHints();
  ComPtr<CoreWebView2EnvironmentOptions> options =
      Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
  if (options) options->put_AdditionalBrowserArguments(kWebView2Arguments);
  const auto key = std::make_shared<std::wstring>(requestedKey);
  const HRESULT started = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, folderForCreation.c_str(), options.Get(),
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [this, key](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            Complete(*key, FAILED(result) || !environment
                               ? (FAILED(result) ? result : E_POINTER)
                               : S_OK,
                     environment);
            return S_OK;
          }).Get());
  if (FAILED(started)) Complete(requestedKey, started, nullptr);
}

void SharedWebViewEnvironment::Complete(const std::wstring& key, HRESULT result,
                                        ICoreWebView2Environment* environment) {
  std::vector<Completion> callbacks;
  ComPtr<ICoreWebView2Environment> readyEnvironment;
  {
    std::lock_guard lock(mutex_);
    auto iterator = entries_.find(key);
    if (iterator == entries_.end()) return;
    Entry& entry = iterator->second;
    entry.creating = false;
    if (SUCCEEDED(result) && environment) {
      entry.environment = environment;
      readyEnvironment = entry.environment;
    }
    callbacks.swap(entry.pending);
  }

  for (auto& callback : callbacks) {
    callback(result, readyEnvironment.Get());
  }
}
}
