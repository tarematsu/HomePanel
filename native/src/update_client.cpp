#include "update_client.h"

#include <bcrypt.h>
#include <softpub.h>
#include <wintrust.h>
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {
constexpr int kHttpTimeoutMs = 30'000;

struct InternetHandle {
  HINTERNET value = nullptr;
  ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
  explicit InternetHandle(HINTERNET handle = nullptr) : value(handle) {}
  InternetHandle(const InternetHandle&) = delete;
  InternetHandle& operator=(const InternetHandle&) = delete;
};

bool IsHexSha256(const std::wstring& value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](wchar_t character) {
    return (character >= L'0' && character <= L'9') ||
           (character >= L'a' && character <= L'f') ||
           (character >= L'A' && character <= L'F');
  });
}

std::optional<std::array<int, 4>> ParseVersionParts(const std::wstring& value) {
  if (value.empty()) return std::nullopt;
  if (value.size() == 10 &&
      std::all_of(value.begin(), value.end(), [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; })) {
    return std::array<int, 4>{
        std::stoi(value.substr(0, 2)),
        std::stoi(value.substr(2, 2)),
        std::stoi(value.substr(4, 2)),
        std::stoi(value.substr(6, 4)),
    };
  }
  std::array<int, 4> result{};
  size_t position = 0;
  size_t partIndex = 0;
  while (position <= value.size() && partIndex < result.size()) {
    const size_t separator = value.find(L'.', position);
    const size_t end = separator == std::wstring::npos ? value.size() : separator;
    if (end == position) return std::nullopt;
    const std::wstring part = value.substr(position, end - position);
    if (!std::all_of(part.begin(), part.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) {
      return std::nullopt;
    }
    try {
      const long parsed = std::stol(part);
      if (parsed < 0 || parsed > 1'000'000) return std::nullopt;
      result[partIndex++] = static_cast<int>(parsed);
    } catch (...) {
      return std::nullopt;
    }
    if (separator == std::wstring::npos) {
      position = value.size();
      break;
    }
    position = separator + 1;
  }
  if (partIndex < 2 || position < value.size()) return std::nullopt;
  return result;
}

bool AllowedName(const std::wstring& value) {
  return value == L"HomePanel.exe" || value == L"HomePanelUpdater.exe" || value == L"WebView2Loader.dll";
}
}  // namespace

UpdateManifest ParseUpdateManifest(const std::string& json) {
  const auto root = winrt::Windows::Data::Json::JsonObject::Parse(Utf8ToWide(json));
  UpdateManifest manifest;
  manifest.version = root.GetNamedString(L"version", L"").c_str();
  manifest.signedBuild = root.GetNamedBoolean(L"signed", false);
  if (!ParseVersionParts(manifest.version)) throw std::runtime_error("invalid update version");

  std::map<std::wstring, bool> seen;
  const auto files = root.GetNamedArray(L"files");
  for (const auto& value : files) {
    const auto object = value.GetObject();
    UpdateFileSpec file;
    file.name = object.GetNamedString(L"name", L"").c_str();
    file.url = object.GetNamedString(L"url", L"").c_str();
    file.sha256 = object.GetNamedString(L"sha256", L"").c_str();
    std::transform(file.sha256.begin(), file.sha256.end(), file.sha256.begin(), towlower);
    const double size = object.GetNamedNumber(L"size", 0);
    file.size = std::isfinite(size) && size > 0 ? static_cast<uint64_t>(size) : 0;
    file.requireAuthenticode = object.GetNamedBoolean(L"requireAuthenticode", false);
    if (!AllowedName(file.name) || file.url.rfind(L"https://", 0) != 0 || !IsHexSha256(file.sha256) ||
        file.size == 0 || file.size > 64ull * 1024ull * 1024ull || seen[file.name]) {
      throw std::runtime_error("invalid update file manifest");
    }
    seen[file.name] = true;
    manifest.files.push_back(std::move(file));
  }
  if (!seen[L"HomePanel.exe"] || !seen[L"HomePanelUpdater.exe"] || !seen[L"WebView2Loader.dll"] ||
      manifest.files.size() != 3) {
    throw std::runtime_error("incomplete update manifest");
  }
  return manifest;
}

std::vector<uint8_t> DownloadHttpsFile(const std::wstring& url, size_t maximumBytes, const std::wstring& bearerToken) {
  URL_COMPONENTS parts{sizeof(parts)};
  wchar_t host[512]{};
  wchar_t path[4096]{};
  wchar_t extra[2048]{};
  parts.lpszHostName = host;
  parts.dwHostNameLength = _countof(host);
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = _countof(path);
  parts.lpszExtraInfo = extra;
  parts.dwExtraInfoLength = _countof(extra);
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) throw std::runtime_error("invalid update URL");
  if (parts.nScheme != INTERNET_SCHEME_HTTPS) throw std::runtime_error("update URL must use HTTPS");

  const std::wstring hostName(host, parts.dwHostNameLength);
  std::wstring resource(path, parts.dwUrlPathLength);
  if (parts.dwExtraInfoLength && parts.lpszExtraInfo) resource.append(extra, parts.dwExtraInfoLength);

  // Tablets can sit on networks where WPAD/PAC autodetection never resolves,
  // so try automatic proxy detection first and fall back to a direct
  // connection instead of failing the update check outright.
  for (DWORD accessType : {WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_ACCESS_TYPE_NO_PROXY}) {
    InternetHandle session(WinHttpOpen(L"HomePanel-VerifiedUpdate/3.0", accessType,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.value) {
      if (accessType == WINHTTP_ACCESS_TYPE_NO_PROXY) throw std::runtime_error("WinHttpOpen failed");
      continue;
    }
    WinHttpSetTimeouts(session.value, kHttpTimeoutMs, kHttpTimeoutMs, kHttpTimeoutMs, kHttpTimeoutMs);

    InternetHandle connection(WinHttpConnect(session.value, hostName.c_str(), parts.nPort, 0));
    if (!connection.value) {
      if (accessType == WINHTTP_ACCESS_TYPE_NO_PROXY) throw std::runtime_error("WinHttpConnect failed");
      continue;
    }
    InternetHandle request(WinHttpOpenRequest(connection.value, L"GET", resource.c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              WINHTTP_FLAG_SECURE));
    if (!request.value) {
      if (accessType == WINHTTP_ACCESS_TYPE_NO_PROXY) throw std::runtime_error("WinHttpOpenRequest failed");
      continue;
    }
    std::wstring headers;
    if (!bearerToken.empty()) headers = L"Authorization: Bearer " + bearerToken + L"\r\n";
    if (!WinHttpSendRequest(request.value,
                            headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(headers.size()),
                            nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) {
      if (accessType == WINHTTP_ACCESS_TYPE_NO_PROXY) throw std::runtime_error("WinHTTP update download failed");
      continue;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
      throw std::runtime_error("update response status unavailable");
    }

    std::vector<uint8_t> bytes;
    while (true) {
      DWORD available = 0;
      if (!WinHttpQueryDataAvailable(request.value, &available)) throw std::runtime_error("update read failed");
      if (!available) break;
      if (bytes.size() + available > maximumBytes) throw std::runtime_error("update file exceeds size limit");
      const size_t offset = bytes.size();
      bytes.resize(offset + available);
      DWORD read = 0;
      if (!WinHttpReadData(request.value, bytes.data() + offset, available, &read)) {
        throw std::runtime_error("update read failed");
      }
      bytes.resize(offset + read);
    }
    if (status != 200 || bytes.empty()) throw std::runtime_error("update file HTTP " + std::to_string(status));
    return bytes;
  }
  throw std::runtime_error("WinHTTP update download failed");
}

bool IsVersionNewer(const std::wstring& candidate, const std::wstring& current) {
  const auto left = ParseVersionParts(candidate);
  const auto right = ParseVersionParts(current);
  return left && right && *left > *right;
}

std::wstring Sha256Hex(const std::vector<uint8_t>& bytes) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD objectSize = 0;
  DWORD resultSize = 0;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
    throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
  }
  if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
                        sizeof(objectSize), &resultSize, 0) < 0) {
    BCryptCloseAlgorithmProvider(algorithm, 0);
    throw std::runtime_error("BCryptGetProperty failed");
  }
  std::vector<uint8_t> object(objectSize);
  std::array<uint8_t, 32> digest{};
  if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) < 0 ||
      BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) < 0 ||
      BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    throw std::runtime_error("SHA-256 failed");
  }
  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  std::wostringstream output;
  output << std::hex << std::setfill(L'0');
  for (uint8_t value : digest) output << std::setw(2) << static_cast<int>(value);
  return output.str();
}

bool VerifyAuthenticode(const fs::path& path) {
  WINTRUST_FILE_INFO fileInfo{sizeof(fileInfo)};
  fileInfo.pcwszFilePath = path.c_str();
  WINTRUST_DATA data{sizeof(data)};
  data.dwUIChoice = WTD_UI_NONE;
  data.fdwRevocationChecks = WTD_REVOKE_NONE;
  data.dwUnionChoice = WTD_CHOICE_FILE;
  data.pFile = &fileInfo;
  data.dwStateAction = WTD_STATEACTION_VERIFY;
  data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
  GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const LONG status = WinVerifyTrust(nullptr, &policy, &data);
  data.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(nullptr, &policy, &data);
  return status == ERROR_SUCCESS;
}

}  // namespace hp
