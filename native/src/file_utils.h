#pragma once
#include "common.h"

namespace hp::file {

inline bool MatchesText(const fs::path& path, const std::string& content) {
  std::error_code error;
  if (!fs::is_regular_file(path, error) ||
      fs::file_size(path, error) != content.size()) {
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  return input && std::equal(std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>(),
                             content.begin(), content.end());
}

inline bool NonEmpty(const fs::path& path) {
  std::error_code error;
  return fs::is_regular_file(path, error) && fs::file_size(path, error) > 0;
}

inline std::string Stamp(const fs::path& path) {
  std::error_code error;
  const auto size = fs::file_size(path, error);
  if (error) return "missing";
  std::ostringstream stamp;
  stamp << size;
  const auto modified = fs::last_write_time(path, error);
  if (!error) stamp << ':' << modified.time_since_epoch().count();
  return stamp.str();
}

}  // namespace hp::file
