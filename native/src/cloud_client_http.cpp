





#include "cloud_client.h"

namespace hp {

void CloudClient::ResetHttpHandlesLocked() {
  if (connection_) WinHttpCloseHandle(connection_);
  if (session_) WinHttpCloseHandle(session_);
  connection_ = nullptr;
  session_ = nullptr;
  host_.clear();
  basePath_.clear();
  port_ = 0;
}

void CloudClient::EnsureHttpHandlesLocked() {
  if (session_ && connection_) return;
  if (config_.cloudflareBaseUrl.empty()) throw std::runtime_error("cloudflareBaseUrl is empty");

  URL_COMPONENTS parts{sizeof(parts)};
  wchar_t host[256]{};
  wchar_t path[2048]{};
  parts.lpszHostName = host;
  parts.dwHostNameLength = _countof(host);
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = _countof(path);
  if (!WinHttpCrackUrl(config_.cloudflareBaseUrl.c_str(), 0, 0, &parts)) throw std::runtime_error("invalid Cloudflare base URL");
  if (parts.nScheme != INTERNET_SCHEME_HTTPS) throw std::runtime_error("Cloudflare base URL must use HTTPS");

  host_.assign(host, parts.dwHostNameLength);
  basePath_.assign(path, parts.dwUrlPathLength);
  while (!basePath_.empty() && basePath_.back() == L'/') basePath_.pop_back();
  port_ = parts.nPort;
  secure_ = true;

  session_ = WinHttpOpen(L"HomePanel/2.6", accessType_,
                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session_) throw std::runtime_error(WinHttpErrorText("WinHttpOpen"));
  DWORD protocols = WINHTTP_PROTOCOL_FLAG_HTTP2;
  WinHttpSetOption(session_, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &protocols, sizeof(protocols));
  connection_ = WinHttpConnect(session_, host_.c_str(), port_, 0);
  if (!connection_) {
    ResetHttpHandlesLocked();
    throw std::runtime_error(WinHttpErrorText("WinHttpConnect"));
  }
}

HttpResponse CloudClient::Request(const std::wstring& method, const std::wstring& path, const std::wstring& token,
                                  const std::wstring& etag, const std::string& body, const wchar_t* contentType) {
  std::lock_guard lock(httpMutex_);
  for (int attempt = 0; attempt < 2; ++attempt) {
    EnsureHttpHandlesLocked();
    const std::wstring requestPath = basePath_ + path;
    HINTERNET request = WinHttpOpenRequest(connection_, method.c_str(), requestPath.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
    if (!request) {
      ResetHttpHandlesLocked();
      if (accessType_ == WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY) accessType_ = WINHTTP_ACCESS_TYPE_NO_PROXY;
      if (!attempt) continue;
      throw std::runtime_error(WinHttpErrorText("WinHttpOpenRequest"));
    }

    int timeout = 8000;
    WinHttpSetTimeouts(request, timeout, timeout, timeout, timeout);
    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
    std::wstring headers = L"Accept-Encoding: gzip\r\n";
    if (!token.empty()) headers += L"Authorization: Bearer " + token + L"\r\n";
    if (!etag.empty()) headers += L"If-None-Match: " + etag + L"\r\n";
    if (!body.empty()) headers += std::wstring(L"Content-Type: ") + contentType + L"\r\n";

    const BOOL sent = WinHttpSendRequest(
        request, headers.c_str(), static_cast<DWORD>(headers.size()),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
      const DWORD requestError = GetLastError();
      WinHttpCloseHandle(request);
      ResetHttpHandlesLocked();
      if (accessType_ == WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY) {
        accessType_ = WINHTTP_ACCESS_TYPE_NO_PROXY;
        log_.Warn(L"WinHTTP automatic proxy failed; retrying without proxy");
      }
      if (!attempt) continue;
      throw std::runtime_error("WinHTTP request failed (" + std::to_string(requestError) + ")");
    }

    HttpResponse output;
    DWORD statusSize = sizeof(output.status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &output.status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    output.etag = HeaderValue(request, WINHTTP_QUERY_ETAG);
    bool readOk = true;
    bool tooLarge = false;
    for (;;) {
      DWORD available = 0;
      if (!WinHttpQueryDataAvailable(request, &available)) { readOk = false; break; }
      if (!available) break;
      if (output.body.size() + available > kMaxResponseBytes) { tooLarge = true; break; }
      const size_t offset = output.body.size();
      output.body.resize(offset + available);
      DWORD read = 0;
      if (!WinHttpReadData(request, output.body.data() + offset, available, &read)) { readOk = false; break; }
      output.body.resize(offset + read);
      if (!read) break;
    }
    WinHttpCloseHandle(request);
    if (tooLarge) throw std::runtime_error("Cloudflare response exceeded 16 MiB");
    if (!readOk) throw std::runtime_error(WinHttpErrorText("WinHTTP response read"));
    return output;
  }
  throw std::runtime_error("WinHTTP request failed");
}

}
