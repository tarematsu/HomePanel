#include "dashboard_data.h"
#include "json_helpers.h"
#include <winrt/Windows.Data.Json.h>

namespace hp {
namespace {
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValueType;

constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

std::string StringifyUtf8(const JsonObject& object) {
  const winrt::hstring text = object.Stringify();
  if (text.empty()) return {};
  const int inputSize = static_cast<int>(text.size());
  const int size = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), inputSize,
      nullptr, 0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string output(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), inputSize,
      output.data(), size, nullptr, nullptr);
  return output;
}

void AppendRevisionObject(uint64_t& hash, const JsonObject& object) {
  const std::string text = StringifyUtf8(object);
  for (const unsigned char byte : text) {
    hash ^= byte;
    hash *= kFnvPrime;
  }
  hash ^= 0;
  hash *= kFnvPrime;
}

uint64_t SectionRevision(const JsonObject& object) {
  return Fnv1a64(StringifyUtf8(object));
}

uint64_t SectionRevision(const JsonObject& first, const JsonObject& second) {
  uint64_t hash = kFnvOffset;
  AppendRevisionObject(hash, first);
  AppendRevisionObject(hash, second);
  return hash;
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
    wchar_t buffer[24]{};
    swprintf_s(buffer, L" %d%%", static_cast<int>(std::round(battery)));
    state += buffer;
  }
  return state;
}

struct WeatherHourKey {
  int hour;
  const wchar_t* key;
};

constexpr std::array<WeatherHourKey, 12> kWeatherHourOrder{{
    {22, L"22"}, {23, L"23"}, {0, L"0"}, {1, L"1"},
    {2, L"2"}, {3, L"3"}, {4, L"4"}, {5, L"5"},
    {6, L"6"}, {7, L"7"}, {8, L"8"}, {9, L"9"},
}};
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
    next.weatherHours.reserve(kWeatherHourOrder.size());
    for (const WeatherHourKey& entry : kWeatherHourOrder) {
      try {
        if (!hourly.HasKey(entry.key)) continue;
        const auto value = hourly.GetNamedValue(entry.key);
        if (value.ValueType() != JsonValueType::Object) continue;
        const JsonObject item = value.GetObject();
        next.weatherHours.push_back({
            entry.hour,
            json::Text(item, L"icon"),
            NumberOrNaN(item, L"temp"),
            NumberOrNaN(item, L"rainMm"),
        });
      } catch (...) {
      }
    }

    // The native News panel has been removed. Do not materialize up to ten
    // titles/descriptions or stringify the News object solely for a revision
    // that no visible panel consumes. The legacy snapshot fields stay empty so
    // older call sites remain harmless while using no dynamic News storage.

    const JsonObject octopus = json::Object(root, L"octopus");
    next.lastMonthUsage = NumberOrNaN(json::Object(octopus, L"lastMonth"), L"usage");
    next.projectedUsage =
        NumberOrNaN(json::Object(octopus, L"thisMonth"), L"projectedUsage");
    const JsonObject comparison = json::Object(octopus, L"comparison");
    next.currentEnergyLabel = json::Text(comparison, L"currentLabel", L"今週");
    next.previousEnergyLabel = json::Text(comparison, L"previousLabel", L"先週");

    const JsonArray profile = json::Array(octopus, L"profile");
    next.octopusProfile.reserve(7);
    for (uint32_t index = 0;
         index < profile.Size() && next.octopusProfile.size() < 7; ++index) {
      try {
        const auto value = profile.GetAt(index);
        if (value.ValueType() != JsonValueType::Object) continue;
        const JsonObject item = value.GetObject();
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
    next.switchBotDevices.reserve(8);
    for (uint32_t index = 0; index < devices.Size() && index < 8; ++index) {
      try {
        const auto value = devices.GetAt(index);
        if (value.ValueType() != JsonValueType::Object) continue;
        const JsonObject item = value.GetObject();
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
