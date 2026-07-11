#include "dashboard_data.h"
#include "json_helpers.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValueType;

double NumberOrNaN(const JsonObject& object, const wchar_t* name) {
  return json::Number(object, name, std::numeric_limits<double>::quiet_NaN());
}

int IntegerOrZero(const JsonObject& object, const wchar_t* name) {
  const double value = NumberOrNaN(object, name);
  return std::isfinite(value) ? static_cast<int>(std::lround(value)) : 0;
}

PanelDataStatus ReadStatus(const JsonObject& object) {
  PanelDataStatus status;
  const std::wstring value = json::Text(object, L"__status", L"waiting");
  status.state = value == L"ok" ? PanelDataState::Ok
      : value == L"stale" ? PanelDataState::Stale
      : value == L"error" ? PanelDataState::Error
      : PanelDataState::Waiting;
  status.error = json::Text(object, L"__error");
  const double lastSuccess = NumberOrNaN(object, L"__lastSuccessAt");
  if (std::isfinite(lastSuccess)) status.lastSuccessAt = static_cast<int64_t>(lastSuccess);
  return status;
}

void ApplyCloudError(PanelDataStatus& status, const std::wstring& error) {
  if (error.empty()) return;
  status.error = error;
  status.state = status.state == PanelDataState::Ok ? PanelDataState::Stale : PanelDataState::Error;
}

std::wstring Upper(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), towupper);
  return value;
}

std::wstring DeviceState(const JsonObject& item) {
  const std::wstring type = json::Text(item, L"deviceType");
  std::wstring state = L"接続";
  if (type.find(L"Contact") != std::wstring::npos) {
    state = json::Text(item, L"openState", L"-");
  } else if (type.find(L"Motion") != std::wstring::npos || type.find(L"Presence") != std::wstring::npos) {
    state = json::Boolean(item, L"motion") ? L"検知" : L"静止";
  } else if (type.find(L"Plug") != std::wstring::npos) {
    state = Upper(json::Text(item, L"power", L"-"));
    const double watts = NumberOrNaN(item, L"watts");
    if (std::isfinite(watts)) {
      wchar_t buffer[40]{};
      swprintf_s(buffer, L" %.1fW", watts);
      state += buffer;
    }
  }
  const double battery = NumberOrNaN(item, L"battery");
  if (std::isfinite(battery)) {
    state += L" " + std::to_wstring(static_cast<int>(std::round(battery))) + L"%";
  }
  return state;
}
}

bool ParseDashboardSnapshot(const std::string& text, DashboardSnapshot& output, std::wstring* error) {
  try {
    if (text.empty()) {
      if (error) *error = L"dashboard.json is empty";
      return false;
    }

    const JsonObject root = JsonObject::Parse(Utf8ToWide(text));
    DashboardSnapshot next;
    next.loaded = true;
    next.cloudError = json::Text(root, L"__cloudError");

    const JsonObject weather = json::Object(root, L"weather");
    next.weatherStatus = ReadStatus(weather);
    next.city = json::Text(weather, L"city");
    const JsonObject hourly = json::Object(weather, L"hourly");
    for (int hour = 0; hour < 24 && next.weatherHours.size() < 8; ++hour) {
      const std::wstring key = std::to_wstring(hour);
      try {
        if (!hourly.HasKey(key) || hourly.GetNamedValue(key).ValueType() != JsonValueType::Object) continue;
        const JsonObject item = hourly.GetNamedObject(key);
        next.weatherHours.push_back({
            hour,
            json::Text(item, L"icon"),
            NumberOrNaN(item, L"temp"),
            NumberOrNaN(item, L"pop"),
            NumberOrNaN(item, L"rainMm"),
        });
      } catch (...) {
      }
    }

    const JsonObject news = json::Object(root, L"news");
    next.newsStatus = ReadStatus(news);
    const JsonArray newsItems = json::Array(news, L"items");
    for (uint32_t index = 0; index < newsItems.Size() && next.newsItems.size() < 10; ++index) {
      try {
        if (newsItems.GetAt(index).ValueType() != JsonValueType::Object) continue;
        const JsonObject item = newsItems.GetObjectAt(index);
        const std::wstring title = json::Text(item, L"title");
        if (!title.empty()) next.newsItems.push_back({title, json::Text(item, L"description")});
      } catch (...) {
      }
    }
    next.newsItemCount = static_cast<int>(next.newsItems.size());

    const JsonObject octopus = json::Object(root, L"octopus");
    next.octopusStatus = ReadStatus(octopus);
    next.lastMonthUsage = NumberOrNaN(json::Object(octopus, L"lastMonth"), L"usage");
    next.projectedUsage = NumberOrNaN(json::Object(octopus, L"thisMonth"), L"projectedUsage");
    const JsonObject comparison = json::Object(octopus, L"comparison");
    next.currentEnergyIsoYear = IntegerOrZero(comparison, L"currentIsoYear");
    next.currentEnergyIsoWeek = IntegerOrZero(comparison, L"currentIsoWeek");
    next.previousEnergyIsoYear = IntegerOrZero(comparison, L"previousIsoYear");
    next.previousEnergyIsoWeek = IntegerOrZero(comparison, L"previousIsoWeek");
    const JsonArray history = json::Array(octopus, L"history");
    const uint32_t historyStart = history.Size() > 7 ? history.Size() - 7 : 0;
    for (uint32_t index = historyStart; index < history.Size(); ++index) {
      try {
        if (history.GetAt(index).ValueType() != JsonValueType::Object) continue;
        const JsonObject item = history.GetObjectAt(index);
        double previousWeekValue = NumberOrNaN(item, L"previousWeekValue");
        if (!std::isfinite(previousWeekValue)) {
          previousWeekValue = NumberOrNaN(item, L"previousYearValue");
        }
        next.octopusHistory.push_back({
            json::Text(item, L"weekday"),
            json::Text(item, L"date"),
            NumberOrNaN(item, L"value"),
            json::Text(item, L"previousWeekDate", json::Text(item, L"previousYearDate")),
            previousWeekValue,
        });
      } catch (...) {
      }
    }

    const JsonObject switchbot = json::Object(root, L"switchbot");
    next.switchBotStatus = ReadStatus(switchbot);
    next.switchBotPresence = json::Text(switchbot, L"presence", L"unknown");
    next.switchBotBrightness = json::Text(switchbot, L"brightness", L"unknown");
    next.switchBotDoorOpen = json::Boolean(switchbot, L"doorOpen");
    next.switchBotMotion = json::Boolean(switchbot, L"motion");
    const JsonArray devices = json::Array(switchbot, L"devices");
    for (uint32_t index = 0; index < devices.Size() && index < 8; ++index) {
      try {
        if (devices.GetAt(index).ValueType() != JsonValueType::Object) continue;
        const JsonObject item = devices.GetObjectAt(index);
        next.switchBotDevices.push_back({
            json::Text(item, L"deviceName", json::Text(item, L"deviceId", L"SwitchBot")),
            DeviceState(item),
        });
      } catch (...) {
      }
    }

    ApplyCloudError(next.weatherStatus, next.cloudError);
    ApplyCloudError(next.newsStatus, next.cloudError);
    ApplyCloudError(next.octopusStatus, next.cloudError);
    ApplyCloudError(next.switchBotStatus, next.cloudError);
    output = std::move(next);
    if (error) error->clear();
    return true;
  } catch (const winrt::hresult_error& exception) {
    if (error) *error = exception.message().c_str();
  } catch (const std::exception& exception) {
    if (error) *error = Utf8ToWide(exception.what());
  } catch (...) {
    if (error) *error = L"unknown dashboard parse error";
  }
  return false;
}

bool LoadDashboardSnapshot(const fs::path& path, DashboardSnapshot& output, std::wstring* error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (error) *error = L"dashboard.json not found";
    return false;
  }
  const std::string text((std::istreambuf_iterator<char>(input)), {});
  return ParseDashboardSnapshot(text, output, error);
}

}  // namespace hp
