#pragma once
#include "common.h"

namespace hp {

struct StationheadConfig {
  std::wstring url = L"https://www.stationhead.com/sakuramankai";
  std::wstring fallbackUrl = L"https://www.stationhead.com/buddy46";
  int channelId = 318;
  bool blockImages = true;
  bool blockFonts = true;
  bool lowMemoryMode = true;
  bool secondaryEnabled = true;
  std::wstring secondaryUrl = L"https://www.stationhead.com/buddy46";
};

struct AppConfig {
  std::wstring cloudflareBaseUrl = L"https://homepanel-cloud.example.invalid";
  std::wstring deviceId = L"homepanel-device";
  int screenWidth = 1920;
  int screenHeight = 1280;
  int cloudPollSeconds = 300;
  int telemetryMinutes = 30;
  double temperatureOffset = -4.5;
  std::wstring serialPort;
  StationheadConfig stationhead;
};

AppConfig LoadConfig(const fs::path& path);
std::wstring LoadProtectedToken(const fs::path& path, const wchar_t* environmentName);
bool SaveProtectedToken(const fs::path& path, const std::wstring& value);
}
