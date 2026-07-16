#pragma once
#include "common.h"
#include "sensors.h"
#include "sh.h"

namespace hp {

inline constexpr UINT kRendererActionMessage = WM_APP + 11;

enum class UiAction {
  None,
  AppUpdate,
  Restart,
  StationheadAudioToggle,
  StationheadAudioMute,
};

struct AirHistorySample {
  int64_t timestamp = 0;
  int co2 = 0;
  double temperature = 0;
  double humidity = 0;

  bool operator==(const AirHistorySample&) const = default;
};

struct RenderState {
  SensorSnapshot sensors;
  StationheadStatus stationhead;
  std::wstring appVersion;
  std::vector<AirHistorySample> airHistory;
  std::vector<StationheadPlayHistorySample> stationheadPlayHistory;
  std::wstring toast;
  int newsIndex = 0;
};

}  // namespace hp
