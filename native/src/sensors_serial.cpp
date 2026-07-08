// Part of sensors.cpp's translation unit (see the #include at the end of that
// file). UD-CO2S serial device: locating the COM port and the read loop that
// parses CO2/humidity/temperature samples, applies corrections, and publishes
// them. Uses the MeasurementValuesValid/SampleValuesValid/ConfigurePort/
// PrepareSensor/ReadLine/WriteCommand helpers from sensors.cpp.
#include "sensors.h"

namespace hp {

std::wstring SensorHub::FindSerialPort() {
  if (!config_.serialPort.empty()) return config_.serialPort;
  HDEVINFO devices = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
  if (devices == INVALID_HANDLE_VALUE) return {};
  SP_DEVINFO_DATA info{sizeof(info)};
  std::wstring result;
  std::vector<std::wstring> usb;
  for (DWORD index = 0; SetupDiEnumDeviceInfo(devices, index, &info); ++index) {
    wchar_t friendly[512]{};
    if (!SetupDiGetDeviceRegistryPropertyW(devices, &info, SPDRP_FRIENDLYNAME, nullptr,
                                           reinterpret_cast<PBYTE>(friendly), sizeof(friendly), nullptr)) continue;
    std::wstring name = friendly;
    const auto open = name.rfind(L"(COM");
    const auto close = name.rfind(L')');
    if (open == std::wstring::npos || close <= open) continue;
    const std::wstring port = name.substr(open + 1, close - open - 1);
    std::transform(name.begin(), name.end(), name.begin(), towlower);
    if (name.find(L"ud-co2s") != std::wstring::npos || name.find(L"co2") != std::wstring::npos) {
      result = port;
      break;
    }
    if (name.find(L"usb") != std::wstring::npos) usb.push_back(port);
  }
  SetupDiDestroyDeviceInfoList(devices);
  if (result.empty() && usb.size() == 1) result = usb.front();
  return result;
}

void SensorHub::SerialLoop() {
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  while (!stopping_) {
    const std::wstring port = FindSerialPort();
    if (port.empty()) {
      bool changed = false;
      {
        std::lock_guard lock(mutex_);
        changed = state_.co2Connected || state_.lastError != L"UD-CO2S not found";
        state_.co2Connected = false;
        state_.lastError = L"UD-CO2S not found";
      }
      if (changed) PostMessageW(window_, WM_HP_SENSOR_UPDATED, 0, 0);
      std::unique_lock lock(stopMutex_);
      stopWake_.wait_for(lock, std::chrono::seconds(10), [this] { return stopping_.load(); });
      continue;
    }

    const std::wstring serialPath = port.rfind(L"\\\\.\\", 0) == 0 ? port : L"\\\\.\\" + port;
    HANDLE serial = CreateFileW(serialPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (serial == INVALID_HANDLE_VALUE) {
      {
        std::lock_guard lock(mutex_);
        state_.co2Connected = false;
        state_.lastError = L"UD-CO2S port open failed";
      }
      PostMessageW(window_, WM_HP_SENSOR_UPDATED, 0, 0);
      std::unique_lock lock(stopMutex_);
      stopWake_.wait_for(lock, std::chrono::seconds(10), [this] { return stopping_.load(); });
      continue;
    }

    std::string buffer;
    if (!ConfigurePort(serial) || !PrepareSensor(serial, buffer, stopping_, log_)) {
      CloseHandle(serial);
      {
        std::lock_guard lock(mutex_);
        state_.co2Connected = false;
        state_.lastError = L"UD-CO2S initialization failed";
      }
      PostMessageW(window_, WM_HP_SENSOR_UPDATED, 0, 0);
      std::unique_lock lock(stopMutex_);
      stopWake_.wait_for(lock, std::chrono::seconds(10), [this] { return stopping_.load(); });
      continue;
    }

    {
      std::lock_guard lock(mutex_);
      state_.co2Connected = false;
      state_.lastError = L"UD-CO2S waiting for data";
    }
    PostMessageW(window_, WM_HP_SENSOR_UPDATED, 0, 0);

    auto validSampleDeadline = std::chrono::steady_clock::now() + kSampleTimeout;
    while (!stopping_) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= validSampleDeadline) {
        log_.Warn(L"UD-CO2S produced no valid measurement for 10 seconds");
        break;
      }

      std::string line;
      const LineResult result = ReadLine(serial, buffer, line, stopping_, validSampleDeadline - now);
      if (result != LineResult::Line) {
        if (result == LineResult::Timeout) log_.Warn(L"UD-CO2S measurement timeout");
        else if (result == LineResult::Error) log_.Warn(L"UD-CO2S measurement read failed");
        break;
      }

      int co2 = 0;
      double humidity = 0, temperature = 0;
      if (sscanf_s(line.c_str(), "CO2=%d,HUM=%lf,TMP=%lf", &co2, &humidity, &temperature) != 3 ||
          !MeasurementValuesValid(co2, humidity, temperature)) continue;

      Sample sample;
      sample.observedAt = UnixMillis();
      sample.sequence = std::max(nextSequence_, static_cast<uint64_t>(sample.observedAt));
      nextSequence_ = sample.sequence + 1;
      sample.co2 = co2;
      sample.humidity = humidity;
      sample.temperature = temperature;
      sample.temperatureCorrected = temperature + config_.temperatureOffset;
      const double absolute = 216.7 * (humidity / 100.0 * 6.112 *
          std::exp((17.62 * temperature) / (243.12 + temperature))) / (273.15 + temperature);
      sample.humidityCorrected = std::clamp(
          absolute * (273.15 + sample.temperatureCorrected) /
          (216.7 * 6.112 * std::exp((17.62 * sample.temperatureCorrected) /
          (243.12 + sample.temperatureCorrected))) * 100.0, 0.0, 100.0);
      if (!SampleValuesValid(sample)) continue;
      validSampleDeadline = std::chrono::steady_clock::now() + kSampleTimeout;

      bool visibleChanged = false;
      {
        std::lock_guard lock(mutex_);
        visibleChanged = !state_.co2Connected || state_.co2 != sample.co2 ||
          std::lround(state_.temperatureCorrected * 10) != std::lround(sample.temperatureCorrected * 10) ||
          std::lround(state_.humidityCorrected) != std::lround(sample.humidityCorrected) ||
          !state_.lastError.empty();
        state_.co2Connected = true;
        state_.co2 = sample.co2;
        state_.temperatureRaw = temperature;
        state_.humidityRaw = humidity;
        state_.temperatureCorrected = sample.temperatureCorrected;
        state_.humidityCorrected = sample.humidityCorrected;
        state_.observedAt = sample.observedAt;
        state_.lastError.clear();
      }
      const int64_t bucket = sample.observedAt / kTelemetryBucketMs;
      if (bucket != lastPersistedBucket_ && AppendOutbox(sample)) lastPersistedBucket_ = bucket;
      if (visibleChanged) PostMessageW(window_, WM_HP_SENSOR_UPDATED, 0, 0);
    }

    WriteCommand(serial, "STP");
    CloseHandle(serial);
    {
      std::lock_guard lock(mutex_);
      state_.co2Connected = false;
      state_.lastError = L"UD-CO2S disconnected";
    }
    PostMessageW(window_, WM_HP_SENSOR_UPDATED, 0, 0);
    if (!stopping_) {
      std::unique_lock lock(stopMutex_);
      stopWake_.wait_for(lock, std::chrono::seconds(5), [this] { return stopping_.load(); });
    }
  }
}

}  // namespace hp
