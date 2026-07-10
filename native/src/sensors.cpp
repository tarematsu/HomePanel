#include "sensors.h"
#include <cmath>

namespace hp {
namespace {
constexpr auto kCommandTimeout = std::chrono::seconds(10);
constexpr auto kSampleTimeout = std::chrono::seconds(10);
constexpr size_t kCompactAfterAck = 288;
constexpr uintmax_t kCompactBytes = 1024 * 1024;
constexpr int64_t kEarliestSampleTime = 946'684'800'000;
constexpr int64_t kTelemetryBucketMs = 5 * 60'000;

bool MeasurementValuesValid(int co2, double humidity, double temperature) {
  return co2 >= 250 && co2 <= 10'000 &&
         std::isfinite(humidity) && humidity >= 0.0 && humidity <= 100.0 &&
         std::isfinite(temperature) && temperature >= -40.0 && temperature <= 85.0;
}

bool SampleValuesValid(const SensorHub::Sample& sample) {
  return sample.sequence > 0 && sample.observedAt >= kEarliestSampleTime &&
         MeasurementValuesValid(sample.co2, sample.humidity, sample.temperature) &&
         std::isfinite(sample.temperatureCorrected) &&
         sample.temperatureCorrected >= -80.0 && sample.temperatureCorrected <= 120.0 &&
         std::isfinite(sample.humidityCorrected) &&
         sample.humidityCorrected >= 0.0 && sample.humidityCorrected <= 100.0;
}

enum class LineResult { Line, Timeout, Error, Stopped };

std::string EscapeJson(const std::string& value) {
  std::string out;
  for (char c : value) {
    if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
    else if (c == '\n') out += "\\n";
    else if (static_cast<unsigned char>(c) >= 0x20) out.push_back(c);
  }
  return out;
}

std::string SampleJson(const SensorHub::Sample& s) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(2)
      << "{\"sequence\":" << s.sequence << ",\"observedAt\":" << s.observedAt
      << ",\"co2\":" << s.co2 << ",\"temperature\":" << s.temperature
      << ",\"humidity\":" << s.humidity << ",\"temperatureCorrected\":" << s.temperatureCorrected
      << ",\"humidityCorrected\":" << s.humidityCorrected << '}';
  return out.str();
}

bool ConfigurePort(HANDLE serial) {
  DCB dcb{};
  dcb.DCBlength = sizeof(dcb);
  if (!GetCommState(serial, &dcb)) return false;
  dcb.BaudRate = CBR_115200;
  dcb.ByteSize = 8;
  dcb.Parity = NOPARITY;
  dcb.StopBits = ONESTOPBIT;
  dcb.fBinary = TRUE;
  dcb.fParity = FALSE;
  dcb.fOutxCtsFlow = FALSE;
  dcb.fOutxDsrFlow = FALSE;
  dcb.fDtrControl = DTR_CONTROL_ENABLE;
  dcb.fDsrSensitivity = FALSE;
  dcb.fTXContinueOnXoff = TRUE;
  dcb.fOutX = FALSE;
  dcb.fInX = FALSE;
  dcb.fErrorChar = FALSE;
  dcb.fNull = FALSE;
  dcb.fRtsControl = RTS_CONTROL_ENABLE;
  dcb.fAbortOnError = FALSE;
  if (!SetCommState(serial, &dcb)) return false;

  COMMTIMEOUTS timeouts{};
  timeouts.ReadIntervalTimeout = MAXDWORD;
  timeouts.ReadTotalTimeoutMultiplier = 0;
  timeouts.ReadTotalTimeoutConstant = 1000;
  timeouts.WriteTotalTimeoutMultiplier = 0;
  timeouts.WriteTotalTimeoutConstant = 1000;
  if (!SetCommTimeouts(serial, &timeouts)) return false;
  SetupComm(serial, 4096, 4096);
  return PurgeComm(serial, PURGE_RXABORT | PURGE_RXCLEAR | PURGE_TXABORT | PURGE_TXCLEAR) != FALSE;
}

LineResult ReadLine(HANDLE serial, std::string& buffer, std::string& line,
                    const std::atomic<bool>& stopping, std::chrono::steady_clock::duration timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!stopping.load()) {
    const size_t newline = buffer.find('\n');
    if (newline != std::string::npos) {
      line = buffer.substr(0, newline);
      buffer.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      return LineResult::Line;
    }
    if (std::chrono::steady_clock::now() >= deadline) return LineResult::Timeout;
    char chunk[256]{};
    DWORD read = 0;
    if (!ReadFile(serial, chunk, sizeof(chunk), &read, nullptr)) return LineResult::Error;
    if (read) {
      buffer.append(chunk, read);
      if (buffer.size() > 16 * 1024) return LineResult::Error;
    }
  }
  return LineResult::Stopped;
}

bool WriteCommand(HANDLE serial, const char* command) {
  const std::string payload = std::string(command) + "\r\n";
  DWORD written = 0;
  return WriteFile(serial, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) && written == payload.size();
}

bool PrepareSensor(HANDLE serial, std::string& buffer, const std::atomic<bool>& stopping, Logger& log) {
  for (const char* command : {"STP", "ID?", "STA"}) {
    if (!WriteCommand(serial, command)) return false;
    Sleep(100);
    const auto deadline = std::chrono::steady_clock::now() + kCommandTimeout;
    for (;;) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        log.Warn(L"UD-CO2S command timeout: " + Utf8ToWide(command));
        return false;
      }
      std::string line;
      const LineResult result = ReadLine(serial, buffer, line, stopping, deadline - now);
      if (result != LineResult::Line) {
        log.Warn(L"UD-CO2S command timeout/read failure: " + Utf8ToWide(command));
        return false;
      }
      if (line.rfind("OK", 0) == 0) break;
      if (line.rfind("NG", 0) == 0) {
        log.Warn(L"UD-CO2S rejected command: " + Utf8ToWide(command));
        return false;
      }
    }
  }
  return true;
}


}

SensorHub::SensorHub(HWND window, AppConfig config, fs::path dataDir, Logger& log)
    : window_(window), config_(std::move(config)), switchbotPath_(dataDir / L"switchbot.json"),
      outboxPath_(dataDir / L"outbox.ndjson"), outboxAckPath_(std::move(dataDir) / L"outbox.ack"), log_(log) {
  LoadOutbox();
  nextSequence_ = std::max(nextSequence_, static_cast<uint64_t>(std::max<int64_t>(1, UnixMillis())));
}

SensorHub::~SensorHub() { Stop(); }

void SensorHub::Start() {
  stopping_ = false;
  ApplyCloudSwitchBot(switchbotPath_);
  serialThread_ = std::thread([this] { SerialLoop(); });
  log_.Info(L"SwitchBot input uses Cloudflare/OpenAPI; native BLE watcher is disabled");
}

void SensorHub::Stop() {
  stopping_ = true;
  stopWake_.notify_all();
  if (serialThread_.joinable()) serialThread_.join();
}

SensorSnapshot SensorHub::Snapshot() const {
  std::lock_guard lock(mutex_);
  return state_;
}

void SensorHub::ApplyCloudSwitchBot(const fs::path& path) {
  try {
    std::ifstream input(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(input)), {});
    if (text.empty()) return;
    const auto object = winrt::Windows::Data::Json::JsonObject::Parse(Utf8ToWide(text));
    const std::wstring presence = object.GetNamedString(L"presence", L"unknown").c_str();
    const std::wstring brightness = object.GetNamedString(L"brightness", L"unknown").c_str();
    {
      std::lock_guard lock(mutex_);
      state_.doorOpen = object.GetNamedBoolean(L"doorOpen", false);
      state_.motion = object.GetNamedBoolean(L"motion", false);
      if (brightness == L"bright") state_.light = true;
      else if (brightness == L"dim") state_.light = false;
      state_.presence = presence == L"home" ? PresenceState::Home :
                        presence == L"away" ? PresenceState::Away : PresenceState::Unknown;
    }
  } catch (const std::exception& error) {
    log_.Warn(L"SwitchBot cloud state error: " + Utf8ToWide(error.what()));
  } catch (...) {
    log_.Warn(L"SwitchBot cloud state parse failed");
  }
}

}




#include "sensors_serial.cpp"
#include "sensors_outbox.cpp"
