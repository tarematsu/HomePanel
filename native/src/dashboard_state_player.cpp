#include "web_renderer.h"

namespace hp::statejson {
namespace {
std::wstring Quote(const std::wstring& value) {
  std::wstring output = L"\"";
  for (wchar_t c : value) {
    if (c == L'\\' || c == L'\"') output.push_back(L'\\');
    if (c == L'\n') output += L"\\n";
    else if (c != L'\r') output.push_back(c);
  }
  return output + L"\"";
}
}

std::wstring Player(const StationheadStatus& value) {
  auto quote = [](const std::wstring& text) {
    return Quote(text);
  };
  std::wostringstream json;
  json << L"{\"created\":" << (value.created ? L"true" : L"false")
       << L",\"navigating\":" << (value.navigating ? L"true" : L"false")
       << L",\"playing\":" << (value.playing ? L"true" : L"false")
       << L",\"audioPlaying\":" << (value.audioPlaying ? L"true" : L"false")
       << L",\"loginRequired\":" << (value.loginRequired ? L"true" : L"false")
       << L",\"lightweight\":" << (value.lightweight ? L"true" : L"false")
       << L",\"visible\":" << (value.visible ? L"true" : L"false")
       << L",\"processFailed\":" << (value.processFailed ? L"true" : L"false")
       << L",\"spotifyConfigured\":" << (value.spotifyConfigured ? L"true" : L"false")
       << L",\"authAvailable\":" << (value.authAvailable ? L"true" : L"false")
       << L",\"audioSilent\":" << (value.audioSilent ? L"true" : L"false")
       << L",\"sampledAt\":" << value.sampledAt
       << L",\"expectedEndAt\":" << value.expectedEndAt
       << L",\"trackDurationMs\":" << value.trackDurationMs
       << L",\"detail\":" << Quote(value.detail)
       << L",\"trackTitle\":" << quote(value.trackTitle)
       << L",\"trackArtist\":" << quote(value.trackArtist)
       << L",\"deviceName\":" << quote(value.deviceName)
       << L",\"artworkUrl\":" << quote(value.artworkUrl)
       << L",\"url\":" << Quote(value.url) << L"}";
  return json.str();
}
}  // namespace hp::statejson
