#include "web_renderer.h"

namespace hp::statejson {
std::wstring Player(const StationheadStatus& value) {
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
       << L",\"audioMuted\":" << (value.audioMuted ? L"true" : L"false")
       << L",\"secondaryAudioMuted\":" << (value.secondaryAudioMuted ? L"true" : L"false")
       << L",\"sampledAt\":" << value.sampledAt
       << L",\"expectedEndAt\":" << value.expectedEndAt
       << L",\"trackDurationMs\":" << value.trackDurationMs
       << L",\"detail\":" << JsonQuote(value.detail)
       << L",\"trackTitle\":" << JsonQuote(value.trackTitle)
       << L",\"trackArtist\":" << JsonQuote(value.trackArtist)
       << L",\"deviceName\":" << JsonQuote(value.deviceName)
       << L",\"artworkUrl\":" << JsonQuote(value.artworkUrl)
       << L",\"url\":" << JsonQuote(value.url) << L"}";
  return json.str();
}
}  // namespace hp::statejson
