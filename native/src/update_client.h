#pragma once
#include "common.h"

namespace hp {

struct UpdateFileSpec {
  std::wstring name;
  std::wstring url;
  std::wstring sha256;
  uint64_t size = 0;
  bool requireAuthenticode = false;
};

struct UpdateManifest {
  std::wstring version;
  bool signedBuild = false;
  std::vector<UpdateFileSpec> files;
};

UpdateManifest ParseUpdateManifest(const std::string& json);
std::vector<uint8_t> DownloadHttpsFile(const std::wstring& url, size_t maximumBytes, const std::wstring& bearerToken = {});
bool IsVersionNewer(const std::wstring& candidate, const std::wstring& current);
std::wstring Sha256Hex(const std::vector<uint8_t>& bytes);
bool VerifyAuthenticode(const fs::path& path);

}
