#include <curl/curl.h>

#include <algorithm>
#include <auralog/curl_transport.hpp>
#include <iostream>
#include <mutex>

namespace auralog {
namespace {

std::once_flag curl_init_once;

std::string trim_endpoint(std::string endpoint) {
  while (!endpoint.empty() && endpoint.back() == '/') {
    endpoint.pop_back();
  }
  return endpoint;
}

}  // namespace

CurlTransport::CurlTransport(Config config) : config_(std::move(config)) {
  config_.endpoint = trim_endpoint(config_.endpoint);
  std::call_once(curl_init_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  curl_ = curl_easy_init();
}

CurlTransport::~CurlTransport() {
  if (curl_ != nullptr) {
    curl_easy_cleanup(curl_);
  }
}

SendResult CurlTransport::send_batch(const std::vector<LogEntry>& entries) {
  nlohmann::json logs = nlohmann::json::array();
  for (const auto& entry : entries) {
    logs.push_back(entry.to_wire());
  }
  return post_json("/v1/logs", {{"projectApiKey", config_.api_key}, {"logs", logs}});
}

SendResult CurlTransport::send_single(const LogEntry& entry) {
  return post_json("/v1/logs/single",
                   {{"projectApiKey", config_.api_key}, {"log", entry.to_wire()}});
}

SendResult CurlTransport::post_json(const std::string& path, const nlohmann::json& body) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (curl_ == nullptr) {
    return SendResult::RetryableFailure;
  }

  curl_easy_reset(curl_);
  const auto url = config_.endpoint + path;
  const auto payload = body.dump();
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
  curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS,
                   static_cast<long>(config_.http_timeout.count()));
  curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, static_cast<long>(config_.http_timeout.count()));
  curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);

  const CURLcode code = curl_easy_perform(curl_);
  long status = 0;
  curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);

  if (code != CURLE_OK) {
    std::cerr << "auralog: libcurl delivery failure: " << curl_easy_strerror(code) << '\n';
    return SendResult::RetryableFailure;
  }
  if (status >= 200 && status < 300) {
    return SendResult::Success;
  }
  if (status >= 400 && status < 500) {
    std::cerr << "auralog: non-retryable HTTP " << status << " from ingest\n";
    return SendResult::PermanentFailure;
  }
  std::cerr << "auralog: retryable HTTP " << status << " from ingest\n";
  return SendResult::RetryableFailure;
}

}  // namespace auralog
