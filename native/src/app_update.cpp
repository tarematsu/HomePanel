// Part of app.cpp's translation unit (see the #include at the end of that
// file). Self-update: async manifest check and launching the verified,
// signature/hash-checked HomePanelUpdater. Uses the InstalledHomePanelVersion /
// Quote helpers from app.cpp.
#include "app.h"

namespace hp {

void App::CheckForUpdateAsync(bool install) {
  if (updateBusy_.exchange(true)) {
    if (install) {
      renderState_.toast = L"更新確認はすでに実行中です";
      toastUntil_ = UnixMillis() + 4000;
      MarkRenderStateDirty();
      InvalidateAll();
    }
    return;
  }
  if (updateThread_.joinable()) updateThread_.join();
  if (install) {
    renderState_.toast = L"署名・ハッシュを確認して更新を準備しています";
    toastUntil_ = UnixMillis() + 15'000;
    MarkRenderStateDirty();
    InvalidateAll();
  }

  updateThread_ = std::thread([this, install] {
    std::wstring message;
    try {
      const std::string manifestJson = cloud_->FetchUpdateManifest();
      const UpdateManifest manifest = ParseUpdateManifest(manifestJson);
      const std::wstring currentVersion = [] (const fs::path& rootDir) {
        const std::wstring installed = InstalledHomePanelVersion(rootDir / L"HomePanel.exe");
        return installed.empty() ? std::wstring(kVersion) : installed;
      }(rootDir_);
      if (!IsVersionNewer(manifest.version, currentVersion)) {
        if (install) {
          message = L"すでに最新バージョンです (v" + currentVersion + L")";
        }
      } else if (!install) {
        message = L"HomePanel " + manifest.version + L" が利用できます";
      } else if (LaunchVerifiedUpdater(manifest.version, manifestJson)) {
        logger_->Info(L"Verified updater launched for version " + manifest.version);
        PostMessageW(window_, WM_CLOSE, 0, 0);
        updateBusy_ = false;
        return;
      } else {
        message = L"検証済み更新プログラムを起動できませんでした";
      }
    } catch (const std::exception& error) {
      logger_->Warn(L"Update check failed: " + Utf8ToWide(error.what()));
      if (install) message = L"更新確認に失敗: " + Utf8ToWide(error.what());
    }

    if (!message.empty()) {
      auto copy = std::make_unique<wchar_t[]>(message.size() + 1);
      wcscpy_s(copy.get(), message.size() + 1, message.c_str());
      if (PostMessageW(window_, WM_HP_UPDATE_RESULT, 0, reinterpret_cast<LPARAM>(copy.get()))) {
        copy.release();
      }
    }
    updateBusy_ = false;
  });
}

bool App::LaunchVerifiedUpdater(const std::wstring& version, const std::string& manifestJson) {
  const fs::path installedUpdater = rootDir_ / L"HomePanelUpdater.exe";
  if (!fs::exists(installedUpdater)) {
    logger_->Warn(L"HomePanelUpdater.exe is not installed; one manual package update is required");
    return false;
  }

  const fs::path pending = dataDir_ / L"pending-update.json";
  const fs::path temporary = dataDir_ / L"pending-update.json.tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(manifestJson.data(), static_cast<std::streamsize>(manifestJson.size()));
    output.flush();
    if (!output) return false;
  }
  if (!MoveFileExW(temporary.c_str(), pending.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temporary.c_str());
    return false;
  }

  const fs::path runnerDirectory = dataDir_ / L"update-runner";
  const fs::path runner = runnerDirectory / L"HomePanelUpdater.exe";
  std::error_code ignored;
  fs::create_directories(runnerDirectory, ignored);
  if (ignored || !CopyFileW(installedUpdater.c_str(), runner.c_str(), FALSE)) {
    logger_->Warn(L"Failed to stage the update runner: " + std::to_wstring(GetLastError()));
    return false;
  }

  std::wstring command = Quote(runner) + L" --pid " + std::to_wstring(GetCurrentProcessId()) +
      L" --root " + Quote(rootDir_) + L" --manifest " + Quote(pending) + L" --version " + version;
  std::vector<wchar_t> buffer(command.begin(), command.end());
  buffer.push_back(L'\0');
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(runner.c_str(), buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, rootDir_.c_str(), &startup, &process)) {
    logger_->Warn(L"CreateProcess for updater failed: " + std::to_wstring(GetLastError()));
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

}  // namespace hp
