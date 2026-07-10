#include "app.h"

namespace {
LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* pointers) {
  if (auto app = hp::App::Current()) app->LogUnhandled(
      pointers && pointers->ExceptionRecord ? pointers->ExceptionRecord->ExceptionCode : 0,
      pointers && pointers->ExceptionRecord ? pointers->ExceptionRecord->ExceptionAddress : nullptr);
  return EXCEPTION_EXECUTE_HANDLER;
}

bool RelaunchSelf() {
  wchar_t executable[MAX_PATH * 4]{};
  if (!GetModuleFileNameW(nullptr, executable, _countof(executable))) return false;

  std::wstring command = L"\"" + std::wstring(executable) + L"\"";
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  const BOOL created = CreateProcessW(
      executable,
      command.data(),
      nullptr,
      nullptr,
      FALSE,
      0,
      nullptr,
      nullptr,
      &startup,
      &process);
  if (!created) return false;
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}
}

int WINAPI wWinMain(
    _In_ HINSTANCE instance,
    _In_opt_ HINSTANCE,
    _In_ LPWSTR,
    _In_ int showCommand) {
  SetUnhandledExceptionFilter(UnhandledFilter);
  winrt::init_apartment(winrt::apartment_type::single_threaded);
  int result = 1;
  try {
    {
      hp::App app(instance);
      result = app.Run(showCommand);
    }
    if (result == 42) result = RelaunchSelf() ? 0 : 1;
  } catch (const std::exception& error) {
    MessageBoxW(nullptr, hp::Utf8ToWide(error.what()).c_str(), L"HomePanel startup failed", MB_ICONERROR | MB_OK);
  }
  winrt::uninit_apartment();
  return result;
}
