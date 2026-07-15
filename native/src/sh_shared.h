#pragma once

#define kStationheadSessionRefreshIntervalMs kStationheadSessionRefreshIntervalMs58
#include "sh_shared_original.h"
#undef kStationheadSessionRefreshIntervalMs

namespace hp {
inline constexpr int64_t kStationheadSessionRefreshIntervalMs = 55 * 60'000;
}
