#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <wincodec.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <shlwapi.h>
#include <setupapi.h>
#include <devguid.h>
#include <regstr.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <shellapi.h>
#include <wrl.h>
#include <WebView2.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <ctime>
#include <exception>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>
#include <string_view>

namespace hp {
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

constexpr wchar_t kAppName[] = L"HomePanel";
constexpr wchar_t kMutexName[] = L"Local\\HomePanelNativeSingleInstance";
constexpr UINT WM_HP_CLOUD_UPDATED = WM_APP + 1;
constexpr UINT WM_HP_SENSOR_UPDATED = WM_APP + 2;
constexpr UINT WM_HP_STATIONHEAD_CHANGED = WM_APP + 3;
constexpr UINT WM_HP_RADAR_UPDATED = WM_APP + 4;
constexpr UINT WM_HP_SWITCHBOT_UPDATED = WM_APP + 5;
constexpr UINT WM_HP_CONFIG_UPDATED = WM_APP + 6;
constexpr UINT WM_HP_COMMANDS_UPDATED = WM_APP + 7;
constexpr UINT WM_HP_PRIMARY_RELOAD_READY = WM_APP + 8;
constexpr UINT WM_HP_SECONDARY_RELOAD_READY = WM_APP + 9;

inline std::wstring Utf8ToWide(const std::string& value) {
  if (value.empty()) return {};
  int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring output(size, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), output.data(), size);
  return output;
}
inline std::string WideToUtf8(const std::wstring& value) {
  if (value.empty()) return {};
  int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string output(size, '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), output.data(), size, nullptr, nullptr);
  return output;
}
inline std::wstring QuotePath(const fs::path& path) {
  return L"\"" + path.wstring() + L"\"";
}
inline std::wstring JsonQuote(const std::wstring& value) {
  std::wstring output = L"\"";
  for (wchar_t c : value) {
    switch (c) {
      case L'\\': output += L"\\\\"; break;
      case L'\"': output += L"\\\""; break;
      case L'\n': output += L"\\n"; break;
      case L'\r': break;
      case L'\t': output += L"\\t"; break;
      default:
        if (c >= 0x20) output.push_back(c);
        break;
    }
  }
  output.push_back(L'\"');
  return output;
}
inline uint64_t Fnv1a64(std::string_view value) noexcept {
  uint64_t hash = 14695981039346656037ull;
  for (unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  return hash;
}
inline std::wstring Hex64(uint64_t value) {
  std::wostringstream output;
  output << std::hex << std::setfill(L'0') << std::setw(16) << value;
  return output.str();
}
inline int64_t UnixMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
inline void ThrowIfFailed(HRESULT hr, const char* where) {
  if (FAILED(hr)) throw std::runtime_error(std::string(where) + " failed: 0x" + [&] { std::ostringstream o; o << std::hex << static_cast<unsigned long>(hr); return o.str(); }());
}

inline const wchar_t* SafeWideGetEnv(const wchar_t* name) noexcept {
  thread_local std::wstring value;
  value.clear();
  if (!name || !*name) return nullptr;

  const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
  if (required == 0) return nullptr;
  value.resize(required);
  const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
  if (written == 0 || written >= required) {
    value.clear();
    return nullptr;
  }
  value.resize(written);
  return value.c_str();
}

inline bool EndsWithInsensitive(const std::wstring& value, const wchar_t* suffix) noexcept {
  if (!suffix) return false;
  const size_t suffixLength = wcslen(suffix);
  if (value.size() < suffixLength) return false;
  return _wcsicmp(value.c_str() + value.size() - suffixLength, suffix) == 0;
}

inline BOOL CopyFileWithActiveUpdaterAwareness(
    LPCWSTR existingFileName, LPCWSTR newFileName, BOOL failIfExists) noexcept {
  if (existingFileName && newFileName) {
    std::wstring destination(newFileName);
    std::replace(destination.begin(), destination.end(), L'/', L'\\');
    if (EndsWithInsensitive(destination, L"\\data\\update-runner\\HomePanelUpdater.exe")) {
      wchar_t currentExecutable[32768]{};
      const DWORD length = GetModuleFileNameW(
          nullptr, currentExecutable, static_cast<DWORD>(std::size(currentExecutable)));
      if (length > 0 && length < std::size(currentExecutable)) {
        const wchar_t* currentName = wcsrchr(currentExecutable, L'\\');
        currentName = currentName ? currentName + 1 : currentExecutable;
        if (_wcsicmp(currentName, L"HomePanel.exe") != 0 &&
            _wcsicmp(currentExecutable, newFileName) != 0) {



          return ::CopyFileW(currentExecutable, newFileName, failIfExists);
        }
      }
    }
  }
  return ::CopyFileW(existingFileName, newFileName, failIfExists);
}


inline bool AtomicWriteBytes(const fs::path& path, const void* data, DWORD size) {
  std::error_code directoryError;
  fs::create_directories(path.parent_path(), directoryError);
  if (directoryError) return false;
  fs::path temp = path.wstring() + L".tmp";
  HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  bool ok = (size == 0) || (WriteFile(file, data, size, &written, nullptr) && written == size);
  if (ok) ok = FlushFileBuffers(file) != FALSE;
  CloseHandle(file);
  if (!ok) {
    DeleteFileW(temp.c_str());
    return false;
  }

  std::error_code existsError;
  const bool targetExists = fs::exists(path, existsError);
  if (existsError) {
    DeleteFileW(temp.c_str());
    return false;
  }
  const bool replaced = targetExists
      ? ReplaceFileW(path.c_str(), temp.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != FALSE
      : MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
  if (!replaced) DeleteFileW(temp.c_str());
  return replaced;
}
inline bool AtomicWriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
  return AtomicWriteBytes(path, bytes.data(), static_cast<DWORD>(bytes.size()));
}
inline bool AtomicWriteText(const fs::path& path, const std::string& text) {
  return AtomicWriteBytes(path, text.data(), static_cast<DWORD>(text.size()));
}
}

#ifdef _MSC_VER
#define _wgetenv(name) (::hp::SafeWideGetEnv(name))
#define CopyFileW(existingFileName, newFileName, failIfExists) \
  (::hp::CopyFileWithActiveUpdaterAwareness((existingFileName), (newFileName), (failIfExists)))
#endif
