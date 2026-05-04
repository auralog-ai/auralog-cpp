#pragma once

#include <auralog/auralog.hpp>

namespace auralog {

class CurlTransport final : public Transport {
 public:
  explicit CurlTransport(Config config);
  SendResult send_batch(const std::vector<LogEntry>& entries) override;
  SendResult send_single(const LogEntry& entry) override;

 private:
  SendResult post_json(const std::string& path, const nlohmann::json& body);

  Config config_;
};

}  // namespace auralog

