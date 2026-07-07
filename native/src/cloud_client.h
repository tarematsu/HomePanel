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
  std::wstring BeginSpotifyAuthorization();
  void StartSpotifyAuthorizationBootstrap();
  bool SpotifyAuthorizationRequired();
  bool IsSpotifyAuthorizationRedirect(const std::wstring& url) const;
  bool CompleteSpotifyAuthorizationRedirect(const std::wstring& url);
  void RefreshSpotifyNow();
  static void RefreshCurrentSpotifySafely() noexcept;
  static void RequestSpotifyPollNow() noexcept;
  std::string FetchUpdateManifest();
  bool SendTelemetry(const std::string& json);
  bool AcknowledgeCommand(int64_t id, bool success, const std::wstring& result);
  std::wstring LastSuccessText() const;
  std::wstring WorkerVersion() const;
  int ConsecutiveFailures() const { return failures_.load(); }
  static CloudClient* Current() { return current_.load(std::memory_order_acquire); }

 private:
  class Registration {
   public:
    explicit Registration(CloudClient* owner) : owner_(owner) {
#ifdef HOMEPANEL_ENABLE_SPOTIFY_RUNTIME
      // CloudClient is created on the UI thread. A one-shot window timer runs only
      // after the constructor has returned to the message loop, so the global
      // Spotify worker can never observe a partially constructed CloudClient.
      if (owner_ && owner_->window_) {
        pendingOwner_.store(owner_, std::memory_order_release);
        timerId_ = SetTimer(
            owner_->window_, reinterpret_cast<UINT_PTR>(owner_), 1,
            &Registration::ActivateTimer);
        if (timerId_) return;
        CloudClient* expected = owner_;
        pendingOwner_.compare_exchange_strong(
            expected, nullptr, std::memory_order_acq_rel,
            std::memory_order_acquire);
      }
#endif
      ActivateNow();
    }

    ~Registration() { Reset(); }

    void Reset() noexcept {
#ifdef HOMEPANEL_ENABLE_SPOTIFY_RUNTIME
      if (timerId_ && owner_ && owner_->window_) {
        KillTimer(owner_->window_, timerId_);
        timerId_ = 0;
      }
      CloudClient* pending = owner_;
      pendingOwner_.compare_exchange_strong(
          pending, nullptr, std::memory_order_acq_rel,
          std::memory_order_acquire);
#endif
      if (!owner_ || !active_) {
        owner_ = nullptr;
        return;
      }
      CloudClient* expected = owner_;
      CloudClient::current_.compare_exchange_strong(
          expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
      // A runtime poll may already have loaded owner_. Wait until that call
      // finishes while all other CloudClient members are still alive.
      std::lock_guard runtimeCall(CloudClient::spotifyRuntimeCallMutex_);
      active_ = false;
      owner_ = nullptr;
    }

   private:
#ifdef HOMEPANEL_ENABLE_SPOTIFY_RUNTIME
    static void CALLBACK ActivateTimer(
        HWND window, UINT, UINT_PTR timerId, DWORD) noexcept {
      KillTimer(window, timerId);
      CloudClient* owner = pendingOwner_.exchange(
          nullptr, std::memory_order_acq_rel);
      if (!owner) return;
      owner->registration_.timerId_ = 0;
      owner->registration_.ActivateNow();
    }
#endif

    void ActivateNow() noexcept {
      if (!owner_ || active_) return;
      active_ = true;
      CloudClient::current_.store(owner_, std::memory_order_release);
#ifdef HOMEPANEL_ENABLE_SPOTIFY_RUNTIME
      CloudClient::RequestSpotifyPollNow();
#endif
    }

#ifdef HOMEPANEL_ENABLE_SPOTIFY_RUNTIME
    static inline std::atomic<CloudClient*> pendingOwner_{nullptr};
#endif
    CloudClient* owner_ = nullptr;
    UINT_PTR timerId_ = 0;
    bool active_ = false;
  };

  HttpResponse Request(const std::wstring& method, const std::wstring& path, const std::wstring& token,
                       const std::wstring& etag = {}, const std::string& body = {}, const wchar_t* contentType = L"application/json");
  void Loop();
  void Synchronize();
  void ApplyPresenceFallback();
  void LoadCacheMetadata();
  void SaveCacheMetadata();
  void EnsureHttpHandlesLocked();
  void ResetHttpHandlesLocked();
  std::vector<uint8_t> LocalizeRadarTiles(const std::vector<uint8_t>& body);

  void SpotifyLoop();
  void PollSpotify();
  bool LoadSpotifyTokens();
  bool SaveSpotifyTokens();
  bool RefreshSpotifyAccessToken();
  bool ExchangeSpotifyCode(const std::wstring& code);
  void WriteSpotifyUnavailable(const std::wstring& error = {});

  static inline std::atomic<CloudClient*> current_{nullptr};
  static inline std::mutex spotifyRuntimeCallMutex_;
  HWND window_;
  AppConfig config_;
  fs::path dataDir_;
  std::wstring deviceToken_;
  std::wstring actionToken_;
  Logger& log_;
  std::thread thread_;
  std::thread spotifyThread_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> immediate_{false};
  std::atomic<bool> spotifyImmediate_{false};
  std::condition_variable wake_;
  std::condition_variable spotifyWake_;
  std::mutex wakeMutex_;
  std::mutex spotifyWakeMutex_;
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

  std::mutex spotifyPollMutex_;
  std::mutex spotifyMutex_;
  std::wstring spotifyClientId_;
  std::wstring spotifyAccessToken_;
  std::wstring spotifyRefreshToken_;
  std::wstring spotifyVerifier_;
  std::wstring spotifyState_;
  int64_t spotifyExpiresAt_ = 0;

  std::wstring lastSuccess_;
  std::wstring workerVersion_;
  int dashboardVersion_ = -1;
  int radarVersion_ = -1;
  int switchbotVersion_ = -1;
  int stationheadVersion_ = -1;
  int deviceConfigVersion_ = -1;
  bool cacheMetadataDirty_ = false;
  bool presenceFallbackActive_ = false;

  // Declared last so it is destroyed first, before any state used by an
  // already-running Spotify poll can disappear.
  Registration registration_{this};
};
}  // namespace hp
