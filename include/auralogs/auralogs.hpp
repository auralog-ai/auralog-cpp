#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace auralogs {

enum class LogLevel { Debug, Info, Warn, Error, Fatal };

std::string to_string(LogLevel level);
std::string level_name(LogLevel level);

struct LogEntry {
  LogLevel level;
  std::string message;
  std::string environment;
  std::string timestamp;
  std::optional<nlohmann::json> metadata;
  std::optional<std::string> stack_trace;
  std::string trace_id;

  nlohmann::json to_wire() const;
};

enum class SendResult { Success, RetryableFailure, PermanentFailure };

class Transport {
 public:
  virtual ~Transport() = default;
  virtual SendResult send_batch(const std::vector<LogEntry>& entries) = 0;
  virtual SendResult send_single(const LogEntry& entry) = 0;
};

struct Config {
  std::string api_key;
  std::string environment = "production";
  std::string endpoint = "https://ingest.auralogs.ai";
  std::chrono::milliseconds flush_interval{5000};
  std::size_t max_batch_size = 50;
  std::size_t max_queue_size = 1000;
  std::size_t max_retry_attempts = 5;
  std::chrono::milliseconds retry_initial_delay{1000};
  std::chrono::milliseconds retry_max_delay{30000};
  std::chrono::milliseconds http_timeout{30000};
  std::chrono::milliseconds shutdown_timeout{2000};
  std::optional<std::string> trace_id;
  std::optional<nlohmann::json> global_metadata;
  std::function<nlohmann::json()> global_metadata_supplier;
  bool capture_terminate = false;
  bool allow_insecure_endpoint = false;
};

class Client : public std::enable_shared_from_this<Client> {
 public:
  static std::shared_ptr<Client> create(Config config);
  static std::shared_ptr<Client> create(Config config, std::shared_ptr<Transport> transport);

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  ~Client();

  void debug(std::string message, nlohmann::json metadata = nlohmann::json::object());
  void info(std::string message, nlohmann::json metadata = nlohmann::json::object());
  void warn(std::string message, nlohmann::json metadata = nlohmann::json::object());
  void error(std::string message, nlohmann::json metadata = nlohmann::json::object(),
             std::optional<std::string> stack_trace = std::nullopt);
  void fatal(std::string message, nlohmann::json metadata = nlohmann::json::object(),
             std::optional<std::string> stack_trace = std::nullopt);
  void log(LogLevel level, std::string message, nlohmann::json metadata = nlohmann::json::object(),
           std::optional<std::string> stack_trace = std::nullopt);

  void flush();
  void flush_for(std::chrono::milliseconds timeout);
  void shutdown();
  void shutdown_for(std::chrono::milliseconds timeout);

  std::string trace_id() const;
  void set_trace_id(std::string trace_id);
  void set_global_metadata(std::optional<nlohmann::json> metadata);
  void set_global_metadata_supplier(std::function<nlohmann::json()> supplier);

 private:
  struct QueuedEntry {
    LogEntry entry;
    std::size_t attempts = 0;
  };

  explicit Client(Config config, std::shared_ptr<Transport> transport);

  void start_worker();
  void worker_loop();
  bool flush_once();
  void flush_until_empty(std::optional<std::chrono::steady_clock::time_point> deadline);
  void enqueue(LogEntry entry);
  void requeue_or_drop(std::vector<QueuedEntry> entries, bool single);
  LogEntry build_entry(LogLevel level, std::string message, nlohmann::json metadata,
                       std::optional<std::string> stack_trace, bool include_global_metadata);
  nlohmann::json merge_metadata(nlohmann::json metadata, bool include_global_metadata);
  void warn_once(const std::string& message);
  bool has_pending_locked() const;

  Config config_;
  std::shared_ptr<Transport> transport_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<QueuedEntry> batch_queue_;
  std::deque<QueuedEntry> single_queue_;
  std::size_t in_flight_count_ = 0;
  bool stopped_ = false;
  bool worker_done_ = false;
  std::unordered_set<std::string> warnings_;
  std::thread worker_;
};

std::shared_ptr<Client> init(Config config);
std::shared_ptr<Client> global();
void shutdown();
void info(std::string message, nlohmann::json metadata = nlohmann::json::object());
void error(std::string message, nlohmann::json metadata = nlohmann::json::object());
void install_terminate_capture(std::shared_ptr<Client> client);

std::string utc_timestamp_millis();
std::string generate_trace_id();

}  // namespace auralogs
