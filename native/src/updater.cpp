#pragma comment(lib, "version.lib")

#include "cloud_client.h"
#include "config.h"
#include "update_client.h"
#include "version.h"

namespace hp {
namespace {
constexpr wchar_t kStandaloneMutexName[] = L"Local\\HomePanelUpdaterStandalone";
constexpr wchar_t kInstallerMutexName[] = L"Local\\HomePanelUpdaterInstaller";
constexpr DWORD kGracefulExitTimeoutMs = 15'000;
constexpr DWORD kForcedExitTimeoutMs = 5'000;

struct ScopedHandle {
  HANDLE value = nullptr;
  ~ScopedHandle() { if (value) CloseHandle(value); }
  ScopedHandle() = default;
  explicit ScopedHandle(HANDLE handle) : value(handle) {}
  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
};

struct Arguments {
  bool runnerMode = false;
  DWORD parentPid = 0;
  DWORD appPid = 0;
  fs::path root;
  fs::path manifest;
  std::wstring expectedVersion;
};

std::wstring InstalledFileVersion(const fs::path& executable) {
  DWORD handle = 0;
  const DWORD size = GetFileVersionInfoSizeW(executable.c_str(), &handle);
  if (!size) return {};
  std::vector<BYTE> data(size);
  if (!GetFileVersionInfoW(executable.c_str(), 0, size, data.data())) return {};
  VS_FIXEDFILEINFO* info = nullptr;
  UINT infoSize = 0;
  if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) ||
      !info || infoSize < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != 0xfeef04bd) {
    return {};
  }
  std::wostringstream version;
  version << HIWORD(info->dwFileVersionMS) << L'.'
          << LOWORD(info->dwFileVersionMS) << L'.'
          << HIWORD(info->dwFileVersionLS);
  const WORD revision = LOWORD(info->dwFileVersionLS);
  if (revision) version << L'.' << revision;
  return version.str();
}

fs::path ExecutableRoot() {
  wchar_t executable[MAX_PATH * 4]{};
  if (!GetModuleFileNameW(nullptr, executable, _countof(executable))) {
    throw std::runtime_error("cannot resolve updater location");
  }
  return fs::weakly_canonical(fs::path(executable).parent_path());
}

Arguments ParseArguments(int argc, wchar_t** argv) {
  Arguments result;
  if (argc == 1) return result;
  if ((argc - 1) % 2 != 0) throw std::runtime_error("invalid updater arguments");
  result.runnerMode = true;
  for (int index = 1; index + 1 < argc; index += 2) {
    const std::wstring key = argv[index];
    const std::wstring value = argv[index + 1];
    if (key == L"--pid") result.parentPid = static_cast<DWORD>(std::stoul(value));
    else if (key == L"--app-pid") result.appPid = static_cast<DWORD>(std::stoul(value));
    else if (key == L"--root") result.root = value;
    else if (key == L"--manifest") result.manifest = value;
    else if (key == L"--version") result.expectedVersion = value;
    else throw std::runtime_error("unknown updater argument");
  }
  if (!result.parentPid || result.root.empty() || result.manifest.empty() || result.expectedVersion.empty()) {
    throw std::runtime_error("invalid updater arguments");
  }
  result.root = fs::weakly_canonical(result.root);
  result.manifest = fs::weakly_canonical(result.manifest);
  const std::wstring dataPrefix = fs::weakly_canonical(result.root / L"data").wstring() + L"\\";
  std::wstring manifestPath = result.manifest.wstring();
  std::wstring expectedPrefix = dataPrefix;
  std::transform(manifestPath.begin(), manifestPath.end(), manifestPath.begin(), towlower);
  std::transform(expectedPrefix.begin(), expectedPrefix.end(), expectedPrefix.begin(), towlower);
  if (manifestPath.rfind(expectedPrefix, 0) != 0) throw std::runtime_error("manifest path is outside HomePanel data");
  return result;
}

void Log(const fs::path& root, const std::wstring& message) {
  fs::create_directories(root / L"data");
  std::wofstream output(root / L"data" / L"homepanel-updater.log", std::ios::app);
  SYSTEMTIME now{};
  GetLocalTime(&now);
  output << L"[" << now.wYear << L"-" << std::setw(2) << std::setfill(L'0') << now.wMonth
         << L"-" << std::setw(2) << now.wDay << L" " << std::setw(2) << now.wHour
         << L":" << std::setw(2) << now.wMinute << L":" << std::setw(2) << now.wSecond
         << L"] " << message << L"\n";
}

std::string ReadManifest(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("pending update manifest not found");
  std::string text((std::istreambuf_iterator<char>(input)), {});
  if (text.empty() || text.size() > 1024 * 1024) throw std::runtime_error("invalid pending update manifest");
  return text;
}

fs::path WritePendingManifest(const fs::path& root, const std::string& text) {
  if (text.empty() || text.size() > 1024 * 1024) throw std::runtime_error("invalid update manifest");
  const fs::path pending = root / L"data" / L"pending-update.json";
  if (!AtomicWriteText(pending, text)) throw std::runtime_error("cannot save update manifest");
  return pending;
}

void WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
  fs::create_directories(path.parent_path());
  const fs::path temporary = path.wstring() + L".download";
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("cannot create staged update file");
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  output.flush();
  if (!output) throw std::runtime_error("cannot finalize staged update file");
  output.close();
  if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temporary.c_str());
    throw std::runtime_error("cannot commit staged update file");
  }
}

void WaitForExit(DWORD pid) {
  if (!pid) return;
  ScopedHandle process(OpenProcess(SYNCHRONIZE, FALSE, pid));
  if (!process.value) return;
  const DWORD wait = WaitForSingleObject(process.value, 90'000);
  if (wait != WAIT_OBJECT_0) throw std::runtime_error("update parent did not exit before timeout");
}

void ReplaceOne(const fs::path& source, const fs::path& target) {
  if (fs::exists(target)) {
    if (!ReplaceFileW(target.c_str(), source.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
      throw std::runtime_error("atomic file replacement failed: " + WideToUtf8(target.filename().wstring()));
    }
  } else if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    throw std::runtime_error("file installation failed: " + WideToUtf8(target.filename().wstring()));
  }
}

void RestartHomePanel(const fs::path& root) {
  const fs::path executable = root / L"HomePanel.exe";
  std::wstring command = QuotePath(executable);
  std::vector<wchar_t> buffer(command.begin(), command.end());
  buffer.push_back(L'\0');
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      root.c_str(), &startup, &process)) {
    throw std::runtime_error("updated HomePanel could not be restarted");
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
}

void RestoreBackup(const fs::path& backup, const fs::path& root) {
  std::error_code ignored;
  if (!fs::exists(backup)) return;
  for (const auto& entry : fs::directory_iterator(backup, ignored)) {
    if (!entry.is_regular_file()) continue;
    fs::copy_file(entry.path(), root / entry.path().filename(), fs::copy_options::overwrite_existing, ignored);
  }
}

std::wstring NormalizedPath(const fs::path& path) {
  std::wstring value = fs::weakly_canonical(path).wstring();
  std::transform(value.begin(), value.end(), value.begin(), towlower);
  return value;
}

bool ProcessMatchesExecutable(HANDLE process, const fs::path& expected) {
  if (!process) return false;
  std::vector<wchar_t> path(32768);
  DWORD length = static_cast<DWORD>(path.size());
  if (!QueryFullProcessImageNameW(process, 0, path.data(), &length)) return false;
  try {
    return NormalizedPath(fs::path(std::wstring(path.data(), length))) == NormalizedPath(expected);
  } catch (...) {
    return false;
  }
}

DWORD FindHomePanelProcess(const fs::path& root) {
  const fs::path expected = root / L"HomePanel.exe";
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W entry{sizeof(entry)};
  DWORD result = 0;
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (_wcsicmp(entry.szExeFile, L"HomePanel.exe") != 0) continue;
      ScopedHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
      if (ProcessMatchesExecutable(process.value, expected)) result = entry.th32ProcessID;
      if (result) break;
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return result;
}

BOOL CALLBACK CloseHomePanelWindow(HWND window, LPARAM parameter) {
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (pid == static_cast<DWORD>(parameter)) PostMessageW(window, WM_CLOSE, 0, 0);
  return TRUE;
}

void RequestHomePanelExit(DWORD pid) {
  if (pid) EnumWindows(CloseHomePanelWindow, static_cast<LPARAM>(pid));
}

void EnsureHomePanelStopped(DWORD pid, const fs::path& root) {
  if (!pid) return;
  const fs::path expected = root / L"HomePanel.exe";
  ScopedHandle process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  if (!process.value || !ProcessMatchesExecutable(process.value, expected)) return;

  const DWORD graceful = WaitForSingleObject(process.value, kGracefulExitTimeoutMs);
  if (graceful == WAIT_OBJECT_0) return;
  if (graceful == WAIT_FAILED) throw std::runtime_error("cannot wait for HomePanel to exit");

  ScopedHandle terminator(OpenProcess(
      PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  if (!terminator.value || !ProcessMatchesExecutable(terminator.value, expected)) {
    throw std::runtime_error("HomePanel is still running and could not be verified for termination");
  }
  if (!TerminateProcess(terminator.value, 1)) {
    throw std::runtime_error("HomePanel is still running and could not be terminated");
  }
  const DWORD forced = WaitForSingleObject(terminator.value, kForcedExitTimeoutMs);
  if (forced != WAIT_OBJECT_0) throw std::runtime_error("HomePanel did not stop after forced termination");
  Log(root, L"Unresponsive HomePanel process was terminated before update");
}

bool LaunchRunner(const fs::path& root, const fs::path& manifest, const std::wstring& version, DWORD appPid) {
  const fs::path installedUpdater = root / L"HomePanelUpdater.exe";
  const fs::path runnerDirectory = root / L"data" / L"update-runner";
  const fs::path runner = runnerDirectory / L"HomePanelUpdater.exe";
  std::error_code ignored;
  fs::create_directories(runnerDirectory, ignored);
  if (ignored || !CopyFileW(installedUpdater.c_str(), runner.c_str(), FALSE)) return false;

  std::wstring command = QuotePath(runner) + L" --pid " + std::to_wstring(GetCurrentProcessId());
  if (appPid) command += L" --app-pid " + std::to_wstring(appPid);
  command += L" --root " + QuotePath(root) + L" --manifest " + QuotePath(manifest) + L" --version " + version;
  std::vector<wchar_t> buffer(command.begin(), command.end());
  buffer.push_back(L'\0');
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(runner.c_str(), buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, root.c_str(), &startup, &process)) {
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

void ValidateManifestVersion(const std::wstring& baselineVersion,
                             const UpdateManifest& manifest,
                             const char* message) {
  if (!baselineVersion.empty() && IsVersionNewer(baselineVersion, manifest.version)) {
    throw std::runtime_error(message);
  }
}

int RunStandalone(const fs::path& root) {
  const fs::path data = root / L"data";
  fs::create_directories(data);
  const std::wstring installedVersion = InstalledFileVersion(root / L"HomePanel.exe");
  const std::wstring baselineVersion = installedVersion.empty()
      ? std::wstring(kVersion)
      : installedVersion;
  Log(root, L"Standalone update check started for version " + baselineVersion);

  AppConfig config = LoadConfig(data / L"settings.json");
  const std::wstring deviceToken = LoadProtectedToken(data / L"device-token.dat", L"HOMEPANEL_DEVICE_TOKEN");
  if (deviceToken.empty()) {
    throw std::runtime_error("device token is unavailable; start HomePanel once or restore data/device-token.dat");
  }

  Logger cloudLog(data / L"homepanel-updater.log");
  CloudClient cloud(nullptr, config, data, deviceToken, L"", cloudLog);
  const std::string initialManifestJson = cloud.FetchUpdateManifest();
  const UpdateManifest initialManifest = ParseUpdateManifest(initialManifestJson);
  ValidateManifestVersion(
      baselineVersion, initialManifest,
      "server update version is older than installed HomePanel");

  const bool initiallyNewer = IsVersionNewer(initialManifest.version, baselineVersion);
  const std::wstring prompt = initiallyNewer
      ? L"HomePanel " + initialManifest.version + L" を取得して更新します。\n\n続行しますか？"
      : L"HomePanelは最新版です。\n起動できない場合に備えて、現在の版を再取得して修復しますか？";
  if (MessageBoxW(nullptr, prompt.c_str(), L"HomePanel Updater", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON1) != IDYES) {
    Log(root, L"Standalone update cancelled");
    return 0;
  }

  // The Worker issues download URLs that expire after ten minutes. Refresh after
  // the user confirms so a long-open confirmation dialog cannot invalidate them.
  const std::string manifestJson = cloud.FetchUpdateManifest();
  const UpdateManifest manifest = ParseUpdateManifest(manifestJson);
  ValidateManifestVersion(
      baselineVersion, manifest,
      "server update version became older than installed HomePanel");
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
  Log(root, IsVersionNewer(manifest.version, baselineVersion)
      ? L"Standalone update runner launched" : L"Standalone repair runner launched");
  return 0;
}

void InstallPendingUpdate(const Arguments& arguments) {
  Log(arguments.root, L"Verified update " + arguments.expectedVersion + L" started");
  const UpdateManifest manifest = ParseUpdateManifest(ReadManifest(arguments.manifest));
  if (manifest.version != arguments.expectedVersion) throw std::runtime_error("update version changed before installation");

  const fs::path staging = arguments.root / L"data" / L"update-staging" / manifest.version;
  const fs::path backup = arguments.root / L"data" / L"update-backup";
  const std::wstring deviceToken = LoadProtectedToken(arguments.root / L"data" / L"device-token.dat", L"HOMEPANEL_DEVICE_TOKEN");
  if (deviceToken.empty()) {
    throw std::runtime_error("device token is unavailable; cannot download authenticated update files");
  }
  fs::remove_all(staging);
  fs::create_directories(staging);

  for (const auto& file : manifest.files) {
    const size_t maximum = static_cast<size_t>(std::min<uint64_t>(file.size + 1024 * 1024, 64ull * 1024ull * 1024ull));
    const auto bytes = DownloadHttpsFile(file.url, maximum, deviceToken);
    if (bytes.size() != file.size) throw std::runtime_error("update file size mismatch");
    std::wstring actual = Sha256Hex(bytes);
    std::transform(actual.begin(), actual.end(), actual.begin(), towlower);
    if (actual != file.sha256) throw std::runtime_error("update file SHA-256 mismatch");
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
  for (const wchar_t* name : {L"HomePanel.exe", L"HomePanelUpdater.exe", L"WebView2Loader.dll"}) {
    const fs::path current = arguments.root / name;
    if (fs::exists(current)) fs::copy_file(current, backup / name, fs::copy_options::overwrite_existing);
  }

  try {
    ReplaceOne(staging / L"WebView2Loader.dll", arguments.root / L"WebView2Loader.dll");
    ReplaceOne(staging / L"HomePanelUpdater.exe", arguments.root / L"HomePanelUpdater.exe");
    ReplaceOne(staging / L"HomePanel.exe", arguments.root / L"HomePanel.exe");
  } catch (...) {
    RestoreBackup(backup, arguments.root);
    throw;
  }

  std::error_code ignored;
  fs::remove(arguments.manifest, ignored);
  fs::remove_all(staging, ignored);
  Log(arguments.root, L"Verified update installed; restarting HomePanel");
  RestartHomePanel(arguments.root);
}
}  // namespace
}  // namespace hp

int WINAPI wWinMain(
    _In_ HINSTANCE,
    _In_opt_ HINSTANCE,
    _In_ LPWSTR,
    _In_ int) {
  hp::fs::path root;
  bool runnerMode = false;
  try {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) throw std::runtime_error("command line parsing failed");
    hp::Arguments arguments = hp::ParseArguments(argc, argv);
    LocalFree(argv);

    runnerMode = arguments.runnerMode;
    const wchar_t* mutexName = runnerMode ? hp::kInstallerMutexName : hp::kStandaloneMutexName;
    hp::ScopedHandle mutex(CreateMutexW(nullptr, TRUE, mutexName));
    const DWORD mutexError = GetLastError();
    if (!mutex.value) throw std::runtime_error("cannot create updater instance lock");
    if (mutexError == ERROR_ALREADY_EXISTS) {
      if (!runnerMode) {
        MessageBoxW(nullptr, L"HomePanel Updaterはすでに実行中です。",
                    L"HomePanel Updater", MB_ICONINFORMATION | MB_OK);
      }
      return 0;
    }

    if (!runnerMode) {
      root = hp::ExecutableRoot();
      return hp::RunStandalone(root);
    }

    root = arguments.root;
    hp::InstallPendingUpdate(arguments);
    return 0;
  } catch (const std::exception& error) {
    const std::wstring message = hp::Utf8ToWide(error.what());
    if (!root.empty()) {
      hp::Log(root, L"Update failed: " + message);
      if (runnerMode) {
        try { hp::RestartHomePanel(root); } catch (...) {}
      }
    }
    MessageBoxW(nullptr, message.c_str(), L"HomePanel update failed", MB_ICONERROR | MB_OK);
    return 1;
  }
}
