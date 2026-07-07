#pragma comment(lib, "version.lib")

#include "cloud_client.h"
#include "config.h"

#define RunStandalone LegacyRunStandalone
#define InstallPendingUpdate LegacyInstallPendingUpdate
#define wWinMain HomePanelLegacyUpdaterMain
#include "updater.cpp"
#undef wWinMain
#undef InstallPendingUpdate
#undef RunStandalone

namespace hp {
namespace {

using InstallPresence = std::map<std::wstring, bool>;

std::vector<uint8_t> ReadFileBytes(
    const fs::path& path, uint64_t maximum = 64ull * 1024ull * 1024ull) {
  std::error_code error;
  const uint64_t size = fs::file_size(path, error);
  if (error || size == 0 || size > maximum) {
    throw std::runtime_error(
        "invalid installed update file: " + WideToUtf8(path.filename().wstring()));
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read installed update file");
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  input.read(reinterpret_cast<char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (!input || static_cast<size_t>(input.gcount()) != bytes.size()) {
    throw std::runtime_error("installed update file read failed");
  }
  return bytes;
}

std::string FetchAuthorizedManifest(const fs::path& root) {
  const fs::path data = root / L"data";
  AppConfig config = LoadConfig(data / L"settings.json");
  const std::wstring deviceToken = LoadProtectedToken(data / L"device-token.dat", L"HOMEPANEL_DEVICE_TOKEN");
  if (deviceToken.empty()) {
    throw std::runtime_error("device token is unavailable; start HomePanel once or restore data/device-token.dat");
  }
  Logger cloudLog(data / L"homepanel-updater.log");
  CloudClient cloud(nullptr, config, data, deviceToken, L"", cloudLog);
  return cloud.FetchUpdateManifest();
}

void VerifyInstalledFiles(const UpdateManifest& manifest, const fs::path& root) {
  for (const auto& file : manifest.files) {
    const fs::path installed = root / file.name;
    const auto bytes = ReadFileBytes(installed);
    if (bytes.size() != file.size) {
      throw std::runtime_error(
          "installed update file size mismatch: " + WideToUtf8(file.name));
    }
    std::wstring actual = Sha256Hex(bytes);
    std::transform(actual.begin(), actual.end(), actual.begin(), towlower);
    if (actual != file.sha256) {
      throw std::runtime_error(
          "installed update file SHA-256 mismatch: " + WideToUtf8(file.name));
    }
    if (file.requireAuthenticode && !VerifyAuthenticode(installed)) {
      throw std::runtime_error(
          "installed update file signature verification failed: " + WideToUtf8(file.name));
    }
  }
  const std::wstring installedVersion = InstalledFileVersion(root / L"HomePanel.exe");
  if (installedVersion.empty()) {
    throw std::runtime_error("installed HomePanel version could not be read after update");
  }
}

bool RestoreBackupChecked(
    const fs::path& backup, const fs::path& root,
    const InstallPresence& originalPresence) noexcept {
  try {
    for (const wchar_t* name : {
             L"HomePanel.exe", L"HomePanelUpdater.exe", L"WebView2Loader.dll"}) {
      const fs::path source = backup / name;
      const fs::path target = root / name;
      const auto presence = originalPresence.find(name);
      const bool originallyExisted =
          presence != originalPresence.end() && presence->second;
      std::error_code error;
      if (!originallyExisted) {
        fs::remove(target, error);
        if (error) return false;
        continue;
      }
      if (!fs::exists(source, error) || error) return false;
      fs::copy_file(source, target, fs::copy_options::overwrite_existing, error);
      if (error) return false;
    }
    return true;
  } catch (...) {
    return false;
  }
}

int HardenedRunStandalone(const fs::path& root) {
  const fs::path data = root / L"data";
  fs::create_directories(data);
  const std::wstring installedVersion = InstalledFileVersion(root / L"HomePanel.exe");
  if (installedVersion.empty()) {
    throw std::runtime_error("installed HomePanel version could not be read");
  }
  Log(root, L"Authenticated update check started for installed version " + installedVersion);

  const std::string initialManifestJson = FetchAuthorizedManifest(root);
  const UpdateManifest initialManifest = ParseUpdateManifest(initialManifestJson);
  if (IsVersionNewer(installedVersion, initialManifest.version)) {
    throw std::runtime_error("server update version is older than installed HomePanel");
  }

  const bool initiallyNewer = IsVersionNewer(initialManifest.version, installedVersion);
  const std::wstring prompt = initiallyNewer
      ? L"HomePanelとUpdaterを " + initialManifest.version + L" に更新します。\n\n続行しますか？"
      : L"HomePanelとUpdaterは最新版です。\n現在の版を認証付きの更新配信から再取得して修復しますか？";
  if (MessageBoxW(nullptr, prompt.c_str(), L"HomePanel Updater",
                  MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON1) != IDYES) {
    Log(root, L"Authenticated update cancelled");
    return 0;
  }

  // The Worker issues download URLs that expire after ten minutes. Refresh after
  // the user confirms so a long-open confirmation dialog cannot invalidate them.
  const std::string manifestJson = FetchAuthorizedManifest(root);
  const UpdateManifest manifest = ParseUpdateManifest(manifestJson);
  if (IsVersionNewer(installedVersion, manifest.version)) {
    throw std::runtime_error("server update version became older than installed HomePanel");
  }
  if (manifest.version != initialManifest.version) {
    Log(root, L"Update version changed while awaiting confirmation: " +
              initialManifest.version + L" -> " + manifest.version);
  }

  const fs::path pending = WritePendingManifest(root, manifestJson);
  const DWORD appPid = FindHomePanelProcess(root);
  if (appPid) RequestHomePanelExit(appPid);
  if (!LaunchRunner(root, pending, manifest.version, appPid)) {
    throw std::runtime_error("cannot start the staged updater");
  }
  Log(root, IsVersionNewer(manifest.version, installedVersion)
      ? L"Authenticated update runner launched for HomePanel and HomePanelUpdater"
      : L"Authenticated repair runner launched for HomePanel and HomePanelUpdater");
  return 0;
}

void HardenedInstallPendingUpdate(const Arguments& arguments) {
  Log(arguments.root, L"Verified update " + arguments.expectedVersion + L" started");
  const UpdateManifest manifest = ParseUpdateManifest(ReadManifest(arguments.manifest));
  if (manifest.version != arguments.expectedVersion) {
    throw std::runtime_error("update version changed before installation");
  }

  const std::wstring currentVersion =
      InstalledFileVersion(arguments.root / L"HomePanel.exe");
  if (!currentVersion.empty() && IsVersionNewer(currentVersion, manifest.version)) {
    throw std::runtime_error("refusing to downgrade installed HomePanel");
  }

  const fs::path staging =
      arguments.root / L"data" / L"update-staging" / manifest.version;
  const fs::path backup = arguments.root / L"data" / L"update-backup";
  const std::wstring deviceToken = LoadProtectedToken(arguments.root / L"data" / L"device-token.dat", L"HOMEPANEL_DEVICE_TOKEN");
  if (deviceToken.empty()) {
    throw std::runtime_error("device token is unavailable; cannot download authenticated update files");
  }
  fs::remove_all(staging);
  fs::create_directories(staging);

  for (const auto& file : manifest.files) {
    const size_t maximum = static_cast<size_t>(
        std::min<uint64_t>(file.size + 1024 * 1024, 64ull * 1024ull * 1024ull));
    const auto bytes = DownloadHttpsFile(file.url, maximum, deviceToken);
    if (bytes.size() != file.size) {
      throw std::runtime_error("update file size mismatch");
    }
    std::wstring actual = Sha256Hex(bytes);
    std::transform(actual.begin(), actual.end(), actual.begin(), towlower);
    if (actual != file.sha256) {
      throw std::runtime_error("update file SHA-256 mismatch");
    }
    const fs::path staged = staging / file.name;
    WriteBytes(staged, bytes);
    if (file.requireAuthenticode && !VerifyAuthenticode(staged)) {
      throw std::runtime_error("update file Authenticode verification failed");
    }
  }

  WaitForExit(arguments.parentPid);
  EnsureHomePanelStopped(arguments.appPid, arguments.root);
  fs::remove_all(backup);
  fs::create_directories(backup);

  InstallPresence originalPresence;
  for (const wchar_t* name : {
           L"HomePanel.exe", L"HomePanelUpdater.exe", L"WebView2Loader.dll"}) {
    const fs::path current = arguments.root / name;
    std::error_code error;
    const bool existed = fs::exists(current, error);
    if (error) throw std::runtime_error("cannot inspect update target");
    originalPresence.emplace(name, existed);
    if (!existed) continue;
    fs::copy_file(current, backup / name,
                  fs::copy_options::overwrite_existing, error);
    if (error) throw std::runtime_error("cannot create update backup");
  }

  try {
    ReplaceOne(staging / L"WebView2Loader.dll",
               arguments.root / L"WebView2Loader.dll");
    ReplaceOne(staging / L"HomePanelUpdater.exe",
               arguments.root / L"HomePanelUpdater.exe");
    ReplaceOne(staging / L"HomePanel.exe",
               arguments.root / L"HomePanel.exe");
    VerifyInstalledFiles(manifest, arguments.root);
  } catch (...) {
    const bool restored =
        RestoreBackupChecked(backup, arguments.root, originalPresence);
    if (!restored) {
      Log(arguments.root, L"Update failed and backup restoration also failed");
      throw std::runtime_error(
          "update failed and backup restoration failed; manual package recovery is required");
    }
    Log(arguments.root,
        L"Update failed; previous files and original file presence restored successfully");
    throw;
  }

  std::error_code ignored;
  fs::remove(arguments.manifest, ignored);
  fs::remove_all(staging, ignored);
  Log(arguments.root,
      L"Verified authenticated update installed and re-verified; restarting HomePanel");
  RestartHomePanel(arguments.root);
}

}  // namespace
}  // namespace hp

namespace {

std::filesystem::path CurrentExecutablePath() {
  wchar_t buffer[32768]{};
  const DWORD length =
      GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
  if (!length || length >= std::size(buffer)) return {};
  return std::filesystem::path(std::wstring(buffer, length));
}

bool LooksLikeHomePanelRoot(const std::filesystem::path& root) {
  std::error_code error;
  return std::filesystem::is_regular_file(root / L"HomePanel.exe", error) &&
         (std::filesystem::is_directory(root / L"data", error) ||
          std::filesystem::is_regular_file(root / L"config.example.json", error));
}

std::filesystem::path ResolveHomePanelRoot(
    const std::filesystem::path& executable) {
  std::vector<std::filesystem::path> candidates;
  if (!executable.empty()) {
    candidates.push_back(executable.parent_path());
    candidates.push_back(executable.parent_path().parent_path());
  }
  wchar_t current[32768]{};
  const DWORD currentLength =
      GetCurrentDirectoryW(static_cast<DWORD>(std::size(current)), current);
  if (currentLength && currentLength < std::size(current)) {
    candidates.emplace_back(std::wstring(current, currentLength));
  }
  if (const wchar_t* systemDrive = _wgetenv(L"SystemDrive");
      systemDrive && *systemDrive) {
    candidates.emplace_back(std::filesystem::path(systemDrive) / L"HomePanel");
  }
  candidates.emplace_back(L"C:\\HomePanel");
  for (const auto& candidate : candidates) {
    try {
      const auto normalized = std::filesystem::weakly_canonical(candidate);
      if (LooksLikeHomePanelRoot(normalized)) return normalized;
    } catch (...) {
    }
  }
  return {};
}

int RelaunchFromHomePanelRoot(
    const std::filesystem::path& source,
    const std::filesystem::path& root) {
  const std::filesystem::path bootstrap =
      root / L"HomePanelUpdater.bootstrap.exe";
  if (!CopyFileW(source.c_str(), bootstrap.c_str(), FALSE)) {
    MessageBoxW(nullptr,
                L"HomePanelのフォルダーへUpdaterを準備できませんでした。\n"
                L"書き込み権限とウイルス対策ソフトの隔離状況を確認してください。",
                L"HomePanel Updater", MB_ICONERROR | MB_OK);
    return 1;
  }

  std::wstring command = L"\"" + bootstrap.wstring() + L"\"";
  std::vector<wchar_t> commandBuffer(command.begin(), command.end());
  commandBuffer.push_back(L'\0');
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(bootstrap.c_str(), commandBuffer.data(), nullptr, nullptr,
                      FALSE, 0, nullptr, root.c_str(), &startup, &process)) {
    DeleteFileW(bootstrap.c_str());
    MessageBoxW(nullptr, L"HomePanel Updaterを起動できませんでした。",
                L"HomePanel Updater", MB_ICONERROR | MB_OK);
    return 1;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return 0;
}

}  // namespace

int WINAPI wWinMain(
    _In_ HINSTANCE,
    _In_opt_ HINSTANCE,
    _In_ LPWSTR,
    _In_ int) {
  hp::fs::path root;
  bool runnerMode = false;
  try {
    SetErrorMode(
        SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) throw std::runtime_error("command line parsing failed");
    hp::Arguments arguments = hp::ParseArguments(argc, argv);
    LocalFree(argv);

    runnerMode = arguments.runnerMode;
    if (!runnerMode) {
      const std::filesystem::path executable = CurrentExecutablePath();
      if (executable.empty()) {
        throw std::runtime_error("cannot resolve updater location");
      }
      if (!LooksLikeHomePanelRoot(executable.parent_path())) {
        const std::filesystem::path resolved =
            ResolveHomePanelRoot(executable);
        if (resolved.empty()) {
          MessageBoxW(
              nullptr,
              L"HomePanelの設置先を特定できませんでした。\n"
              L"HomePanelUpdater.exeをC:\\HomePanelに置くか、HomePanel.exeと同じフォルダーから実行してください。",
              L"HomePanel Updater", MB_ICONERROR | MB_OK);
          return 1;
        }
        return RelaunchFromHomePanelRoot(executable, resolved);
      }
      root = executable.parent_path();
    } else {
      root = arguments.root;
    }

    const wchar_t* mutexName =
        runnerMode ? hp::kInstallerMutexName : hp::kStandaloneMutexName;
    hp::ScopedHandle mutex(CreateMutexW(nullptr, TRUE, mutexName));
    const DWORD mutexError = GetLastError();
    if (!mutex.value) {
      throw std::runtime_error("cannot create updater instance lock");
    }
    if (mutexError == ERROR_ALREADY_EXISTS) {
      if (!runnerMode) {
        MessageBoxW(nullptr, L"HomePanel Updaterはすでに実行中です。",
                    L"HomePanel Updater", MB_ICONINFORMATION | MB_OK);
      }
      return 0;
    }

    int result = 0;
    if (!runnerMode) {
      result = hp::HardenedRunStandalone(root);
      const std::filesystem::path executable = CurrentExecutablePath();
      if (_wcsicmp(executable.filename().c_str(),
                   L"HomePanelUpdater.bootstrap.exe") == 0) {
        MoveFileExW(executable.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
      }
      return result;
    }

    hp::HardenedInstallPendingUpdate(arguments);
    return 0;
  } catch (const std::exception& error) {
    const std::wstring message = hp::Utf8ToWide(error.what());
    if (!root.empty()) {
      hp::Log(root, L"Update failed: " + message);
      if (runnerMode) {
        try {
          hp::RestartHomePanel(root);
        } catch (...) {
        }
      }
    }
    MessageBoxW(nullptr, message.c_str(), L"HomePanel update failed",
                MB_ICONERROR | MB_OK);
    return 1;
  }
}
