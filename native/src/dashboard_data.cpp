#include "dashboard_data.h"
#include "json_helpers.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValueType;

void AppendRevisionSource(std::string& source, const JsonObject& object) {
  source += WideToUtf8(std::wstring(object.Stringify().c_str()));
  source.push_back('\0');
}

uint64_t SectionRevision(const JsonObject& object) {
  return Fnv1a64(WideToUtf8(std::wstring(object.Stringify().c_str())));
}

uint64_t SectionRevision(const JsonObject& first, const JsonObject& second) {
  std::string source;
  AppendRevisionSource(source, first);
  AppendRevisionSource(source, second);
  return Fnv1a64(source);
}

double NumberOrNaN(const JsonObject& object, const wchar_t* name) {
  return json::Number(object, name, std::numeric_limits<double>::quiet_NaN());
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
  } else if (type.find(L"Motion") != std::wstring::npos ||
             type.find(L"Presence") != std::wstring::npos) {
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
}  // namespace

bool ParseDashboardSnapshot(
    const std::string& text, DashboardSnapshot& output, std::wstring* error) {
  try {
    if (text.empty()) {
      if (error) *error = L"dashboard.json is empty";
      return false;
    }

    const JsonObject root = JsonObject::Parse(Utf8ToWide(text));
    DashboardSnapshot next;
    next.loaded = true;

    const JsonObject weather = json::Object(root, L"weather");
    next.revisions.weather = SectionRevision(weather);
    const JsonObject hourly = json::Object(weather, L"hourly");
    static constexpr std::array<int, 12> kWeatherHourOrder{
        22, 23, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    for (const int hour : kWeatherHourOrder) {
      const std::wstring key = std::to_wstring(hour);
      try {
        if (!hourly.HasKey(key) ||
            hourly.GetNamedValue(key).ValueType() != JsonValueType::Object) {
          continue;
        }
        const JsonObject item = hourly.GetNamedObject(key);
        next.weatherHours.push_back({
            hour,
            json::Text(item, L"icon"),
            NumberOrNaN(item, L"temp"),
            NumberOrNaN(item, L"rainMm"),
        });
      } catch (...) {
      }
    }

    const JsonObject news = json::Object(root, L"news");
    next.revisions.news = SectionRevision(news);
    const JsonArray newsItems = json::Array(news, L"items");
    for (uint32_t index = 0;
         index < newsItems.Size() && next.newsItems.size() < 10; ++index) {
      try {
        if (newsItems.GetAt(index).ValueType() != JsonValueType::Object) continue;
        const JsonObject item = newsItems.GetObjectAt(index);
        const std::wstring title = json::Text(item, L"title");
        if (!title.empty()) {
          next.newsItems.push_back({title, json::Text(item, L"description")});
        }
      } catch (...) {
      }
    }
    next.newsItemCount = static_cast<int>(next.newsItems.size());

    const JsonObject octopus = json::Object(root, L"octopus");
    next.lastMonthUsage = NumberOrNaN(json::Object(octopus, L"lastMonth"), L"usage");
    next.projectedUsage =
        NumberOrNaN(json::Object(octopus, L"thisMonth"), L"projectedUsage");
    const JsonObject comparison = json::Object(octopus, L"comparison");
    next.currentEnergyLabel = json::Text(comparison, L"currentLabel", L"今週");
    next.previousEnergyLabel = json::Text(comparison, L"previousLabel", L"先週");

    const JsonArray profile = json::Array(octopus, L"profile");
    for (uint32_t index = 0;
         index < profile.Size() && next.octopusProfile.size() < 7; ++index) {
      try {
        if (profile.GetAt(index).ValueType() != JsonValueType::Object) continue;
        const JsonObject item = profile.GetObjectAt(index);
        const std::wstring day = json::Text(item, L"day");
        if (day.empty()) continue;
        const bool currentComplete = json::Boolean(item, L"currentComplete");
        const bool previousComplete = json::Boolean(item, L"previousComplete");
        double currentTotal = NumberOrNaN(item, L"currentTotal");
        double previousTotal = NumberOrNaN(item, L"previousTotal");
        if (!currentComplete) currentTotal = std::numeric_limits<double>::quiet_NaN();
        if (!previousComplete) previousTotal = std::numeric_limits<double>::quiet_NaN();
        next.octopusProfile.push_back(OctopusProfileData{
            day, currentTotal, previousTotal, currentComplete, previousComplete});
      } catch (...) {
      }
    }

    const JsonObject switchbot = json::Object(root, L"switchbot");
    next.revisions.energy = SectionRevision(octopus, switchbot);
    const JsonArray devices = json::Array(switchbot, L"devices");
    for (uint32_t index = 0; index < devices.Size() && index < 8; ++index) {
      try {
        if (devices.GetAt(index).ValueType() != JsonValueType::Object) continue;
        const JsonObject item = devices.GetObjectAt(index);
        next.switchBotDevices.push_back({
            json::Text(item, L"deviceName",
                       json::Text(item, L"deviceId", L"SwitchBot")),
            DeviceState(item),
        });
      } catch (...) {
      }
    }

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

}  // namespace hp
