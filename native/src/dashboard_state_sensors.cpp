#include "web_renderer.h"

namespace hp::statejson {
namespace {
std::wstring Presence(PresenceState value) {
  if (value == PresenceState::Home) return L"home";
  if (value == PresenceState::Away) return L"away";
  return L"unknown";
}
}

std::wstring Sensors(const SensorSnapshot& value) {
  std::wostringstream json;
  json << L"{\"connected\":" << (value.co2Connected ? L"true" : L"false")
       << L",\"co2\":" << value.co2
       << L",\"temperature\":" << value.temperatureCorrected
       << L",\"humidity\":" << value.humidityCorrected
       << L",\"presence\":" << JsonQuote(Presence(value.presence))
       << L",\"light\":" << (value.light ? L"true" : L"false")
       << L",\"motion\":" << (value.motion ? L"true" : L"false")
       << L",\"doorOpen\":" << (value.doorOpen ? L"true" : L"false")
       << L",\"outboxCount\":" << value.outboxCount
       << L",\"lastError\":" << JsonQuote(value.lastError) << L"}";
  return json.str();
}
}  // namespace hp::statejson
