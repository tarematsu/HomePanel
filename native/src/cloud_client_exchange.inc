namespace hp {

void CloudClient::QueueTelemetry(std::string json, TelemetryCallback callback) {
  if (json.empty()) return;
  std::lock_guard lock(pendingTelemetryMutex_);
  pendingTelemetry_ = PendingTelemetry{
      ++pendingTelemetryGeneration_, std::move(json), std::move(callback)};
}

std::optional<HttpResponse> CloudClient::TryCompressedExchange(
    const std::wstring& method,
    const std::wstring& path,
    const std::wstring& token) {
  if (method == L"GET" && path.starts_with(L"/v1/radar/bundle/")) {
    std::lock_guard lock(pendingExchangeMutex_);
    const std::wstring requested = path.substr(0, path.find_first_of(L"?#"));
    if (!pendingExchangeRadar_.empty() && requested == pendingExchangeBundlePath_) {
      HttpResponse response;
      response.status = 200;
      response.body = std::move(pendingExchangeRadar_);
      pendingExchangeBundlePath_.clear();
      return response;
    }
    return std::nullopt;
  }
  if (method != L"GET" || !path.starts_with(L"/v1/device/sync?")) {
    return std::nullopt;
  }

  const auto number = [&](const wchar_t* name) {
    const std::wstring value = ExchangeQueryValue(path, name);
    return value.empty() ? std::string("-1") : WideToUtf8(value);
  };
  const std::wstring deviceId = ExchangeQueryValue(path, L"deviceId");
  std::string body = "{\"versions\":{";
  body += "\"dashboard\":" + number(L"dashboardVersion");
  body += ",\"radar\":" + number(L"radarVersion");
  body += ",\"switchbot\":" + number(L"switchbotVersion");
  body += ",\"stationhead\":" + number(L"stationheadVersion");
  body += ",\"stationheadHealth\":" + number(L"stationheadHealthVersion");
  body += ",\"config\":" + number(L"configVersion") + "}";

  std::optional<PendingTelemetry> telemetry;
  {
    std::lock_guard lock(pendingTelemetryMutex_);
    telemetry = pendingTelemetry_;
  }
  if (telemetry && !telemetry->json.empty()) body += ",\"telemetry\":" + telemetry->json;
  body.push_back('}');

  const HttpResponse exchange = Request(
      L"POST", L"/v1/device/exchange?deviceId=" + deviceId,
      token, {}, body, L"application/json");
  if (exchange.status == 404 || exchange.status == 405) return std::nullopt;
  if (exchange.status != 200) return exchange;
  if (exchange.body.size() < 12 || std::memcmp(exchange.body.data(), "HPEX0001", 8) != 0) {
    throw std::runtime_error("device exchange header invalid");
  }
  const uint32_t jsonLength = ExchangeReadUint32(exchange.body, 8);
  const size_t jsonEnd = 12 + static_cast<size_t>(jsonLength);
  if (jsonLength == 0 || jsonEnd > exchange.body.size()) {
    throw std::runtime_error("device exchange JSON length invalid");
  }

  HttpResponse translated;
  translated.status = 200;
  translated.body.assign(exchange.body.begin() + 12, exchange.body.begin() + jsonEnd);
  const JsonObject root = JsonObject::Parse(Utf8ToWide(std::string(
      reinterpret_cast<const char*>(translated.body.data()), translated.body.size())));

  std::wstring bundlePath;
  if (root.HasKey(L"radar") && root.GetNamedValue(L"radar").ValueType() == JsonValueType::String) {
    try {
      const JsonObject radar = JsonObject::Parse(root.GetNamedString(L"radar"));
      bundlePath = radar.GetNamedString(L"bundleUrl", L"").c_str();
      const size_t separator = bundlePath.find_first_of(L"?#");
      if (separator != std::wstring::npos) bundlePath.resize(separator);
    } catch (...) {
      bundlePath.clear();
    }
  }
  {
    std::lock_guard lock(pendingExchangeMutex_);
    pendingExchangeBundlePath_ = std::move(bundlePath);
    pendingExchangeRadar_.assign(exchange.body.begin() + jsonEnd, exchange.body.end());
    if (pendingExchangeBundlePath_.empty()) pendingExchangeRadar_.clear();
  }

  if (telemetry && root.HasKey(L"telemetry") &&
      root.GetNamedValue(L"telemetry").ValueType() == JsonValueType::Object) {
    TelemetryReceipt receipt = ParseExchangeTelemetryReceipt(root.GetNamedObject(L"telemetry"));
    if (receipt.success) {
      TelemetryCallback callback;
      {
        std::lock_guard lock(pendingTelemetryMutex_);
        if (pendingTelemetry_ && pendingTelemetry_->generation == telemetry->generation) {
          callback = std::move(pendingTelemetry_->callback);
          pendingTelemetry_.reset();
        }
      }
      if (callback) callback(std::move(receipt));
    }
  }
  return translated;
}

}  // namespace hp
