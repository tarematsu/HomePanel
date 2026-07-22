#pragma once
#include "common.h"
namespace hp {
class Logger {
 public:
  explicit Logger(fs::path path, size_t maxBytes = 2 * 1024 * 1024, int rotations = 3);
  ~Logger();
  void Info(const std::wstring& message) { Write(L"INFO", message); }
  void Warn(const std::wstring& message) { Write(L"WARN", message); }
  void Error(const std::wstring& message) { Write(L"ERROR", message); }
  fs::path Path() const { return path_; }
 private:
  void Write(const wchar_t* level, const std::wstring& message);
  void OpenOutput();
  void Rotate();
  fs::path path_;
  size_t maxBytes_;
  int rotations_;
  size_t currentBytes_ = 0;
  size_t pendingBytes_ = 0;
  std::ofstream output_;
  std::mutex mutex_;
};
}  // namespace hp
