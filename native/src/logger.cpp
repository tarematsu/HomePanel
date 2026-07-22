#include "logger.h"

namespace hp {
namespace {
constexpr size_t kLogFlushThresholdBytes = 64 * 1024;
}

Logger::Logger(fs::path path, size_t maxBytes, int rotations)
    : path_(std::move(path)), maxBytes_(maxBytes), rotations_(rotations) {
  fs::create_directories(path_.parent_path());
  std::error_code error;
  currentBytes_ = static_cast<size_t>(fs::file_size(path_, error));
  if (error) currentBytes_ = 0;
  if (maxBytes_ > 0 && currentBytes_ >= maxBytes_) {
    Rotate();
  } else {
    OpenOutput();
  }
}

Logger::~Logger() {
  std::lock_guard lock(mutex_);
  if (output_.is_open()) {
    output_.flush();
    output_.close();
  }
}

void Logger::OpenOutput() {
  if (output_.is_open()) return;
  output_.open(path_, std::ios::binary | std::ios::app);
}

void Logger::Rotate() {
  if (output_.is_open()) {
    output_.flush();
    output_.close();
  }

  std::error_code error;
  if (rotations_ <= 0) {
    fs::remove(path_, error);
  } else {
    fs::path oldest = path_;
    oldest += L"." + std::to_wstring(rotations_);
    fs::remove(oldest, error);
    error.clear();

    for (int index = rotations_ - 1; index >= 1; --index) {
      fs::path from = path_;
      from += L"." + std::to_wstring(index);
      fs::path to = path_;
      to += L"." + std::to_wstring(index + 1);
      if (!fs::exists(from, error) || error) {
        error.clear();
        continue;
      }
      fs::remove(to, error);
      error.clear();
      fs::rename(from, to, error);
      error.clear();
    }

    fs::path first = path_;
    first += L".1";
    fs::remove(first, error);
    error.clear();
    fs::rename(path_, first, error);
  }

  currentBytes_ = 0;
  pendingBytes_ = 0;
  OpenOutput();
}

void Logger::Write(const wchar_t* level, const std::wstring& message) {
  SYSTEMTIME time{};
  GetLocalTime(&time);
  char header[32]{};
  sprintf_s(header, "[%04u-%02u-%02u %02u:%02u:%02u] ",
            time.wYear, time.wMonth, time.wDay,
            time.wHour, time.wMinute, time.wSecond);

  std::string line = header;
  line += WideToUtf8(level);
  line.push_back(' ');
  line += WideToUtf8(message);
  line.push_back('\n');

  std::lock_guard lock(mutex_);
  if (maxBytes_ > 0 && currentBytes_ > 0 &&
      line.size() > maxBytes_ - std::min(currentBytes_, maxBytes_)) {
    Rotate();
  }
  OpenOutput();
  if (!output_.is_open()) return;

  output_.write(line.data(), static_cast<std::streamsize>(line.size()));
  if (!output_) {
    output_.close();
    return;
  }
  currentBytes_ += line.size();
  pendingBytes_ += line.size();

  const bool important = _wcsicmp(level, L"INFO") != 0;
  if (important || pendingBytes_ >= kLogFlushThresholdBytes) {
    output_.flush();
    pendingBytes_ = 0;
  }
}

}  // namespace hp
