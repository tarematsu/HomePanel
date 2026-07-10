#include "config.h"
#include <wincrypt.h>

namespace hp {
namespace {
using winrt::Windows::Data::Json::JsonObject;

std::string ReadAll(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::wstring GetString(const JsonObject& object, const wchar_t* key, const std::wstring& fallback = {}) {
  try { return object.GetNamedString(key, fallback).c_str(); } catch (...) { return fallback; }
}

bool GetBoolean(const JsonObject& object, const wchar_t* key, bool fallback) {
  try { return object.GetNamedBoolean(key, fallback); } catch (...) { return fallback; }
}

int GetInteger(const JsonObject& object, const wchar_t* key, int fallback, int minimum, int maximum) {
  try {
    const double value = object.GetNamedNumber(key, fallback);
    if (!std::isfinite(value)) return fallback;
    return static_cast<int>(std::clamp(value, static_cast<double>(minimum), static_cast<double>(maximum)));
  } catch (...) {
    return fallback;
  }
}

JsonObject GetObject(const JsonObject& object, const wchar_t* key) {
  try { return object.GetNamedObject(key); } catch (...) { return JsonObject{}; }
}

std::string Trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::wstring ReadDotEnvValue(const fs::path& path, const std::vector<std::wstring>& keys) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  std::vector<std::string> utf8Keys;
  utf8Keys.reserve(keys.size());
  for (const auto& key : keys) utf8Keys.push_back(WideToUtf8(key));
  std::string line;
  while (std::getline(input, line)) {
    line = Trim(line);
    if (line.empty() || line.front() == '#') continue;
    if (line.rfind("export ", 0) == 0) line = Trim(line.substr(7));
    const auto separator = line.find('=');
    if (separator == std::string::npos) continue;
    const std::string name = Trim(line.substr(0, separator));
    if (std::find(utf8Keys.begin(), utf8Keys.end(), name) == utf8Keys.end()) continue;
    std::string value = Trim(line.substr(separator + 1));
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\''))) {
      value = value.substr(1, value.size() - 2);
    }
    return Utf8ToWide(value);
  }
  return {};
}

std::wstring ReadEnvironmentValue(const std::vector<std::wstring>& keys) {
  for (const auto& key : keys) {
    wchar_t* value = nullptr;
    size_t length = 0;
    if (_wdupenv_s(&value, &length, key.c_str()) == 0 && value && *value) {
      std::wstring result(value);
      free(value);
      return result;
    }
    free(value);
  }
  return {};
}

std::vector<std::wstring> TokenKeys(const wchar_t* environmentName) {
  if (wcscmp(environmentName, L"HOMEPANEL_DEVICE_TOKEN") == 0) {
    return {environmentName, L"DEVICE_TOKEN", L"HOMEPANEL_INGEST_SECRET"};
  }
  if (wcscmp(environmentName, L"HOMEPANEL_ACTION_TOKEN") == 0) {


    return {environmentName, L"API_TOKEN", L"DEVICE_TOKEN", L"HOMEPANEL_INGEST_SECRET"};
  }
  return {environmentName};
}
}

AppConfig LoadConfig(const fs::path& path) {
  AppConfig config;
  const auto text = ReadAll(path);
  if (!text.empty()) {
    try {
      const auto root = JsonObject::Parse(Utf8ToWide(text));
      config.cloudflareBaseUrl = GetString(root, L"cloudflareBaseUrl", config.cloudflareBaseUrl);
      while (!config.cloudflareBaseUrl.empty() && config.cloudflareBaseUrl.back() == L'/') {
        config.cloudflareBaseUrl.pop_back();
      }
      config.deviceId = GetString(root, L"deviceId", config.deviceId);

    } catch (...) {

    }
  }




  if (config.serialPort.empty()) {
    const fs::path root = path.parent_path().parent_path();
    for (const fs::path& envPath : {root / L".env", root / L"cloud" / L".env"}) {
      config.serialPort = ReadDotEnvValue(envPath, {L"HP_CO2_SERIAL_PORT", L"CO2_SERIAL_PORT"});
      if (!config.serialPort.empty()) break;
    }
  }
  return config;
}

bool SaveProtectedToken(const fs::path& path, const std::wstring& value) {
  DATA_BLOB input{static_cast<DWORD>(value.size() * sizeof(wchar_t)),
                  reinterpret_cast<BYTE*>(const_cast<wchar_t*>(value.data()))};
  DATA_BLOB output{};
  if (!CryptProtectData(&input, L"HomePanel device token", nullptr, nullptr, nullptr,
                        CRYPTPROTECT_UI_FORBIDDEN, &output)) return false;
  fs::create_directories(path.parent_path());
  auto temp = path;
  temp += L".tmp";
  HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  bool ok = false;
  if (file != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    ok = WriteFile(file, output.pbData, output.cbData, &written, nullptr) &&
         written == output.cbData && FlushFileBuffers(file);
    CloseHandle(file);
  }
  LocalFree(output.pbData);
  if (!ok) { DeleteFileW(temp.c_str()); return false; }
  if (fs::exists(path)) ok = ReplaceFileW(path.c_str(), temp.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != FALSE;
  else ok = MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH | MOVEFILE_REPLACE_EXISTING) != FALSE;
  return ok;
}

std::wstring LoadProtectedToken(const fs::path& path, const wchar_t* environmentName) {
  const std::vector<std::wstring> keys = TokenKeys(environmentName);
  std::wstring environmentValue = ReadEnvironmentValue(keys);
  if (!environmentValue.empty()) {
    SaveProtectedToken(path, environmentValue);
    return environmentValue;
  }
  {
    std::ifstream input(path, std::ios::binary);
    if (input) {
      std::vector<BYTE> bytes((std::istreambuf_iterator<char>(input)), {});
      DATA_BLOB encrypted{static_cast<DWORD>(bytes.size()), bytes.data()}, plain{};
      if (CryptUnprotectData(&encrypted, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain)) {
        std::wstring value(reinterpret_cast<wchar_t*>(plain.pbData), plain.cbData / sizeof(wchar_t));
        LocalFree(plain.pbData);
        if (!value.empty()) return value;
      }
    }
  }
  const fs::path root = path.parent_path().parent_path();
  for (const fs::path& envPath : {root / L".env", root / L"cloud" / L".env"}) {
    std::wstring value = ReadDotEnvValue(envPath, keys);
    if (!value.empty()) {
      SaveProtectedToken(path, value);
      return value;
    }
  }
  return {};
}
}
