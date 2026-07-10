#include "logger.h"
namespace hp {
Logger::Logger(fs::path path, size_t maxBytes, int rotations) : path_(std::move(path)), maxBytes_(maxBytes), rotations_(rotations) {
  fs::create_directories(path_.parent_path());
}
void Logger::RotateIfNeeded() {
  std::error_code ec;
  if (!fs::exists(path_, ec) || ec || fs::file_size(path_, ec) < maxBytes_ || ec) return;
  if (rotations_ <= 0) {
    fs::remove(path_, ec);
    return;
  }

  fs::path oldest = path_;
  oldest += L"." + std::to_wstring(rotations_);
  fs::remove(oldest, ec);
  ec.clear();

  for (int index = rotations_ - 1; index >= 1; --index) {
    fs::path from = path_;
    from += L"." + std::to_wstring(index);
    fs::path to = path_;
    to += L"." + std::to_wstring(index + 1);
    if (!fs::exists(from, ec) || ec) {
      ec.clear();
      continue;
    }
    fs::remove(to, ec);
    ec.clear();
    fs::rename(from, to, ec);
    ec.clear();
  }

  fs::path first = path_;
  first += L".1";
  fs::remove(first, ec);
  ec.clear();
  fs::rename(path_, first, ec);
}
void Logger::Write(const wchar_t* level, const std::wstring& message) {
  std::lock_guard lock(mutex_);
  RotateIfNeeded();
  SYSTEMTIME time{}; GetLocalTime(&time);



  char header[32]{};
  sprintf_s(header, "[%04u-%02u-%02u %02u:%02u:%02u] ", time.wYear, time.wMonth, time.wDay,
            time.wHour, time.wMinute, time.wSecond);
  std::ofstream output(path_, std::ios::binary | std::ios::app);
  output << header << WideToUtf8(level) << ' ' << WideToUtf8(message) << '\n';
}
}
