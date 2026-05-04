#pragma once

#include <curl/curl.h>

#include <auralog/auralog.hpp>

namespace auralog {

class CurlTransport final : public Transport {
 public:
  explicit CurlTransport(Config config);
  ~CurlTransport() override;

  CurlTransport(const CurlTransport&) = delete;
  CurlTransport& operator=(const CurlTransport&) = delete;

  SendResult send_batch(const std::vector<LogEntry>& entries) override;
  SendResult send_single(const LogEntry& entry) override;

 private:
  SendResult post_json(const std::string& path, const nlohmann::json& body);

  Config config_;
  CURL* curl_ = nullptr;
  std::mutex mutex_;
};

}  // namespace auralog
