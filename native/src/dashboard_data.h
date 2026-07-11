#pragma once
#include "common.h"
#include <limits>

namespace hp {

enum class PanelDataState { Waiting, Ok, Stale, Error };

struct PanelDataStatus {
  PanelDataState state = PanelDataState::Waiting;
  std::wstring error;
  int64_t lastSuccessAt = 0;
};

struct WeatherHourData {
  int hour = 0;
  std::wstring icon;
  double temperature = std::numeric_limits<double>::quiet_NaN();
  double precipitationProbability = std::numeric_limits<double>::quiet_NaN();
  double rainMm = std::numeric_limits<double>::quiet_NaN();
};

struct NewsItemData {
  std::wstring title;
  std::wstring description;
};

struct OctopusProfileData {
  std::wstring time;
  double currentAverage = std::numeric_limits<double>::quiet_NaN();
  double previousAverage = std::numeric_limits<double>::quiet_NaN();
  int currentDays = 0;
  int previousDays = 0;
};

struct SwitchBotDeviceData {
  std::wstring name;
  std::wstring state;
};

struct DashboardSnapshot {
  bool loaded = false;
  std::wstring cloudError;

  PanelDataStatus weatherStatus;
  std::wstring city;
  std::vector<WeatherHourData> weatherHours;

  PanelDataStatus newsStatus;
  std::vector<NewsItemData> newsItems;
  int newsItemCount = 0;

  PanelDataStatus octopusStatus;
  double lastMonthUsage = std::numeric_limits<double>::quiet_NaN();
  double projectedUsage = std::numeric_limits<double>::quiet_NaN();
  std::wstring currentEnergyLabel = L"今週平均";
  std::wstring previousEnergyLabel = L"先週平均";
  std::wstring currentEnergyDateRange;
  std::wstring previousEnergyDateRange;
  std::vector<OctopusProfileData> octopusProfile;

  PanelDataStatus switchBotStatus;
  std::wstring switchBotPresence;
  std::wstring switchBotBrightness;
  bool switchBotDoorOpen = false;
  bool switchBotMotion = false;
  std::vector<SwitchBotDeviceData> switchBotDevices;
};

bool ParseDashboardSnapshot(const std::string& text, DashboardSnapshot& output,
                             std::wstring* error = nullptr);
bool LoadDashboardSnapshot(const fs::path& path, DashboardSnapshot& output, std::wstring* error = nullptr);

}  // namespace hp
