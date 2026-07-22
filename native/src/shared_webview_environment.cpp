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
    L"--disable-features=MediaRouter,Translate,OptimizationGuideModelDownloading,AutofillServerCommunication,HardwareSecureDecryption,HardwareSecureDecryptionExperiment";

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
  uint64_t creationGeneration = 0;
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
        creationGeneration = ++entry.generation;
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
    Complete(requestedKey, creationGeneration,
             HRESULT_FROM_WIN32(directoryError.value()), nullptr);
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
          [this, key, creationGeneration](
              HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            Complete(*key, creationGeneration,
                     FAILED(result) || !environment
                         ? (FAILED(result) ? result : E_POINTER)
                         : S_OK,
                     environment);
            return S_OK;
          }).Get());
  if (FAILED(started)) {
    Complete(requestedKey, creationGeneration, started, nullptr);
  }
}

void SharedWebViewEnvironment::Invalidate(const fs::path& userDataFolder) {
  const std::wstring key = NormalizePath(userDataFolder);
  std::vector<Completion> callbacks;
  {
    std::lock_guard lock(mutex_);
    auto iterator = entries_.find(key);
    if (iterator == entries_.end()) return;
    Entry& entry = iterator->second;
    // A and B share this environment but create independent profile
    // controllers. A timeout after the environment is already ready belongs to
    // that one controller; clearing the shared cache here can make the healthy
    // peer create a second environment against the same user-data folder.
    // Invalidate only an environment creation that is still genuinely pending.
    if (entry.environment) return;
    ++entry.generation;
    entry.creating = false;
    callbacks.swap(entry.pending);
  }
  const HRESULT timeout = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  for (auto& callback : callbacks) callback(timeout, nullptr);
}

void SharedWebViewEnvironment::Complete(const std::wstring& key,
                                        uint64_t generation, HRESULT result,
                                        ICoreWebView2Environment* environment) {
  std::vector<Completion> callbacks;
  ComPtr<ICoreWebView2Environment> readyEnvironment;
  {
    std::lock_guard lock(mutex_);
    auto iterator = entries_.find(key);
    if (iterator == entries_.end()) return;
    Entry& entry = iterator->second;
    if (entry.generation != generation) return;
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
}  // namespace hp
