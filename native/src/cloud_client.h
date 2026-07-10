#pragma once
#include "common.h"
#include "config.h"
#include "logger.h"

namespace hp {
struct HttpResponse {
  DWORD status = 0;
  std::vector<uint8_t> body;
  std::wstring etag;
};

class CloudClient {
 public:
  CloudClient(HWND window, AppConfig config, fs::path dataDir, std::wstring deviceToken, std::wstring actionToken, Logger& log);
  ~CloudClient();
  void Start();
  void Stop();
  void RefreshNow();
  bool RequestRemoteRefresh();
  std::string FetchUpdateManifest();
  bool SendTelemetry(const std::string& json);
  bool AcknowledgeCommand(int64_t id, bool success, const std::wstring& result);
  std::wstring LastSuccessText() const;
  std::wstring WorkerVersion() const;
  std::wstring StationheadHealthText() const;
  int ConsecutiveFailures() const { return failures_.load(); }

 private:
  HttpResponse Request(const std::wstring& method, const std::wstring& path, const std::wstring& token,
                       const std::wstring& etag = {}, const std::string& body = {}, const wchar_t* contentType = L"application/json");
  void Loop();
  void Synchronize();
  void ApplyPresenceFallback();
  void LoadCacheMetadata();
  void SaveCacheMetadata();
  void UpdateStationheadHealthText(std::wstring text);
  void EnsureHttpHandlesLocked();
  void ResetHttpHandlesLocked();
  std::vector<uint8_t> LocalizeRadarTiles(const std::vector<uint8_t>& body);

  void StartNetworkChangeWatcher();
  void StopNetworkChangeWatcher();

  HWND window_;
  AppConfig config_;
  fs::path dataDir_;
  std::wstring deviceToken_;
  std::wstring actionToken_;
  Logger& log_;
  std::thread thread_;
  std::thread networkChangeThread_;
  HANDLE networkChangeStopEvent_{};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> immediate_{false};
  std::condition_variable wake_;
  std::mutex wakeMutex_;
  std::atomic<int> failures_{0};
  mutable std::mutex stateMutex_;

  std::mutex httpMutex_;
  HINTERNET session_ = nullptr;
  HINTERNET connection_ = nullptr;
  std::wstring host_;
  std::wstring basePath_;
  INTERNET_PORT port_ = 0;
  bool secure_ = true;
  DWORD accessType_ = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;

  std::wstring lastSuccess_;
  std::wstring workerVersion_;
  std::wstring stationheadHealthText_ = L"Stationhead収集: 確認中";
  int dashboardVersion_ = -1;
  int radarVersion_ = -1;
  int switchbotVersion_ = -1;
  int stationheadVersion_ = -1;
  int deviceConfigVersion_ = -1;
  bool cacheMetadataDirty_ = false;
  bool presenceFallbackActive_ = false;
};
}
