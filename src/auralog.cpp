#include <algorithm>
#include <auralog/auralog.hpp>
#include <auralog/curl_transport.hpp>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>

namespace auralog {
namespace {

std::mutex global_mutex;
std::weak_ptr<Client> global_client;

bool is_error_or_above(LogLevel level) {
  return level == LogLevel::Error || level == LogLevel::Fatal;
}

}  // namespace

std::string to_string(LogLevel level) { return level_name(level); }

std::string level_name(LogLevel level) {
  switch (level) {
    case LogLevel::Debug:
      return "debug";
    case LogLevel::Info:
      return "info";
    case LogLevel::Warn:
      return "warn";
    case LogLevel::Error:
      return "error";
    case LogLevel::Fatal:
      return "fatal";
  }
  return "info";
}

nlohmann::json LogEntry::to_wire() const {
  nlohmann::json out{{"level", level_name(level)},
                     {"message", message},
                     {"environment", environment},
                     {"timestamp", timestamp},
                     {"traceId", trace_id}};
  if (metadata.has_value()) {
    out["metadata"] = *metadata;
  }
  if (stack_trace.has_value()) {
    out["stackTrace"] = *stack_trace;
  }
  return out;
}

std::shared_ptr<Client> Client::create(Config config) {
  auto transport = std::make_shared<CurlTransport>(config);
  return create(std::move(config), std::move(transport));
}

std::shared_ptr<Client> Client::create(Config config, std::shared_ptr<Transport> transport) {
  if (config.api_key.empty()) {
    throw std::invalid_argument("auralog api_key is required");
  }
  if (config.environment.empty()) {
    throw std::invalid_argument("auralog environment is required");
  }
  if (config.endpoint.empty()) {
    throw std::invalid_argument("auralog endpoint is required");
  }
  if (config.flush_interval.count() <= 0 || config.retry_initial_delay.count() <= 0 ||
      config.retry_max_delay.count() <= 0 || config.http_timeout.count() <= 0 ||
      config.shutdown_timeout.count() <= 0) {
    throw std::invalid_argument("auralog durations must be greater than zero");
  }
  if (config.max_batch_size == 0 || config.max_queue_size == 0 || config.max_retry_attempts == 0) {
    throw std::invalid_argument("auralog queue and retry sizes must be greater than zero");
  }
  if (config.retry_max_delay < config.retry_initial_delay) {
    throw std::invalid_argument("auralog retry_max_delay must be >= retry_initial_delay");
  }
  if (!config.trace_id.has_value()) {
    config.trace_id = generate_trace_id();
  }

  auto client = std::shared_ptr<Client>(new Client(std::move(config), std::move(transport)));
  client->start_worker();
  if (client->config_.capture_terminate) {
    install_terminate_capture(client);
  }
  return client;
}

Client::Client(Config config, std::shared_ptr<Transport> transport)
    : config_(std::move(config)), transport_(std::move(transport)) {}

Client::~Client() { shutdown_for(config_.shutdown_timeout); }

void Client::start_worker() {
  worker_ = std::thread([this] { worker_loop(); });
}

void Client::debug(std::string message, nlohmann::json metadata) {
  log(LogLevel::Debug, std::move(message), std::move(metadata));
}

void Client::info(std::string message, nlohmann::json metadata) {
  log(LogLevel::Info, std::move(message), std::move(metadata));
}

void Client::warn(std::string message, nlohmann::json metadata) {
  log(LogLevel::Warn, std::move(message), std::move(metadata));
}

void Client::error(std::string message, nlohmann::json metadata,
                   std::optional<std::string> stack_trace) {
  log(LogLevel::Error, std::move(message), std::move(metadata), std::move(stack_trace));
}

void Client::fatal(std::string message, nlohmann::json metadata,
                   std::optional<std::string> stack_trace) {
  log(LogLevel::Fatal, std::move(message), std::move(metadata), std::move(stack_trace));
}

void Client::log(LogLevel level, std::string message, nlohmann::json metadata,
                 std::optional<std::string> stack_trace) {
  enqueue(
      build_entry(level, std::move(message), std::move(metadata), std::move(stack_trace), true));
}

void Client::flush() { flush_for(config_.shutdown_timeout); }

void Client::flush_for(std::chrono::milliseconds timeout) {
  flush_until_empty(std::chrono::steady_clock::now() + timeout);
}

void Client::shutdown() { shutdown_for(config_.shutdown_timeout); }

void Client::shutdown_for(std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    cv_.notify_all();
  }
  if (worker_.joinable()) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool done = cv_.wait_until(lock, deadline, [this] { return worker_done_; });
    lock.unlock();
    if (done) {
      worker_.join();
    } else {
      warn_once("auralog: worker did not stop before shutdown timeout; waiting for in-flight work");
      worker_.join();
    }
  }
  flush_until_empty(deadline);
}

std::string Client::trace_id() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return config_.trace_id.value_or("");
}

void Client::set_trace_id(std::string trace_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_.trace_id = std::move(trace_id);
}

void Client::set_global_metadata(std::optional<nlohmann::json> metadata) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_.global_metadata = std::move(metadata);
}

void Client::set_global_metadata_supplier(std::function<nlohmann::json()> supplier) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_.global_metadata_supplier = std::move(supplier);
}

void Client::worker_loop() {
  auto retry_delay = config_.retry_initial_delay;
  while (true) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait_for(lock, config_.flush_interval,
                   [this] { return stopped_ || !single_queue_.empty(); });
      if (stopped_) {
        worker_done_ = true;
        cv_.notify_all();
        return;
      }
    }

    if (flush_once()) {
      retry_delay = config_.retry_initial_delay;
    } else {
      std::this_thread::sleep_for(retry_delay);
      retry_delay = std::min(retry_delay * 2, config_.retry_max_delay);
    }
  }
}

bool Client::flush_once() {
  std::vector<QueuedEntry> entries;
  bool single = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!single_queue_.empty()) {
      entries.push_back(single_queue_.front());
      single_queue_.pop_front();
      single = true;
    } else if (!batch_queue_.empty()) {
      const auto count = std::min(config_.max_batch_size, batch_queue_.size());
      entries.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
        entries.push_back(batch_queue_.front());
        batch_queue_.pop_front();
      }
    } else {
      return true;
    }
    ++in_flight_count_;
  }

  SendResult result = SendResult::Success;
  try {
    if (single) {
      result = transport_->send_single(entries.front().entry);
    } else {
      std::vector<LogEntry> batch;
      batch.reserve(entries.size());
      for (const auto& queued : entries) {
        batch.push_back(queued.entry);
      }
      result = transport_->send_batch(batch);
    }
  } catch (const std::exception& ex) {
    warn_once(std::string("auralog: transport failed: ") + ex.what());
    result = SendResult::RetryableFailure;
  } catch (...) {
    warn_once("auralog: transport failed");
    result = SendResult::RetryableFailure;
  }

  if (result == SendResult::RetryableFailure) {
    requeue_or_drop(std::move(entries), single);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      --in_flight_count_;
      cv_.notify_all();
    }
    return false;
  }
  if (result == SendResult::PermanentFailure) {
    warn_once("auralog: dropping logs after non-retryable delivery failure");
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    --in_flight_count_;
    cv_.notify_all();
  }
  return true;
}

void Client::flush_until_empty(std::optional<std::chrono::steady_clock::time_point> deadline) {
  while (true) {
    if (deadline.has_value() && std::chrono::steady_clock::now() >= *deadline) {
      warn_once("auralog: flush timed out with pending logs");
      return;
    }
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (batch_queue_.empty() && single_queue_.empty() && in_flight_count_ == 0) {
        return;
      }
      if (batch_queue_.empty() && single_queue_.empty()) {
        if (deadline.has_value()) {
          cv_.wait_until(lock, *deadline, [this] { return in_flight_count_ == 0; });
        } else {
          cv_.wait(lock, [this] { return in_flight_count_ == 0; });
        }
        continue;
      }
    }
    const bool success = flush_once();
    if (!success) {
      if (deadline.has_value() &&
          std::chrono::steady_clock::now() + config_.retry_initial_delay > *deadline) {
        warn_once("auralog: retry budget exceeded during flush");
        return;
      }
      std::this_thread::sleep_for(config_.retry_initial_delay);
    }
  }
}

void Client::enqueue(LogEntry entry) {
  const bool immediate = is_error_or_above(entry.level);
  std::lock_guard<std::mutex> lock(mutex_);
  while (batch_queue_.size() + single_queue_.size() >= config_.max_queue_size) {
    if (!batch_queue_.empty()) {
      batch_queue_.pop_front();
    } else {
      single_queue_.pop_front();
    }
  }
  if (immediate) {
    single_queue_.push_back({std::move(entry), 0});
    cv_.notify_all();
  } else {
    batch_queue_.push_back({std::move(entry), 0});
  }
}

void Client::requeue_or_drop(std::vector<QueuedEntry> entries, bool single) {
  bool dropped = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
      it->attempts += 1;
      if (it->attempts >= config_.max_retry_attempts) {
        dropped = true;
        continue;
      }
      if (single) {
        single_queue_.push_front(std::move(*it));
      } else {
        batch_queue_.push_front(std::move(*it));
      }
    }
  }
  if (dropped) {
    warn_once("auralog: dropping logs after retry attempts exhausted");
  }
}

LogEntry Client::build_entry(LogLevel level, std::string message, nlohmann::json metadata,
                             std::optional<std::string> stack_trace, bool include_global_metadata) {
  return LogEntry{level,
                  std::move(message),
                  config_.environment,
                  utc_timestamp_millis(),
                  merge_metadata(std::move(metadata), include_global_metadata),
                  std::move(stack_trace),
                  trace_id()};
}

nlohmann::json Client::merge_metadata(nlohmann::json metadata, bool include_global_metadata) {
  nlohmann::json out = nlohmann::json::object();
  Config snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot.global_metadata = config_.global_metadata;
    snapshot.global_metadata_supplier = config_.global_metadata_supplier;
  }

  if (include_global_metadata) {
    if (snapshot.global_metadata.has_value() && snapshot.global_metadata->is_object()) {
      out.update(*snapshot.global_metadata);
    }
    if (snapshot.global_metadata_supplier) {
      try {
        auto supplied = snapshot.global_metadata_supplier();
        if (supplied.is_object()) {
          out.update(supplied);
        }
      } catch (const std::exception& ex) {
        warn_once(std::string("auralog: global metadata supplier failed: ") + ex.what());
      } catch (...) {
        warn_once("auralog: global metadata supplier failed");
      }
    }
  }

  if (metadata.is_null()) {
    return out;
  }
  if (metadata.is_object()) {
    out.update(metadata);
  } else {
    out["value"] = std::move(metadata);
  }
  return out;
}

void Client::warn_once(const std::string& message) {
  bool should_warn = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    should_warn = warnings_.insert(message).second;
  }
  if (should_warn) {
    std::cerr << message << '\n';
  }
}

bool Client::has_pending_locked() const {
  return !batch_queue_.empty() || !single_queue_.empty() || in_flight_count_ > 0;
}

std::shared_ptr<Client> init(Config config) {
  auto client = Client::create(std::move(config));
  std::lock_guard<std::mutex> lock(global_mutex);
  if (!global_client.expired()) {
    throw std::runtime_error("auralog global client is already initialized");
  }
  global_client = client;
  return client;
}

std::shared_ptr<Client> global() {
  std::lock_guard<std::mutex> lock(global_mutex);
  return global_client.lock();
}

void shutdown() {
  if (auto client = global()) {
    client->shutdown();
  }
}

void info(std::string message, nlohmann::json metadata) {
  if (auto client = global()) {
    client->info(std::move(message), std::move(metadata));
  }
}

void error(std::string message, nlohmann::json metadata) {
  if (auto client = global()) {
    client->error(std::move(message), std::move(metadata));
  }
}

std::string utc_timestamp_millis() {
  const auto now = std::chrono::system_clock::now();
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
  const auto seconds = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &seconds);
#else
  gmtime_r(&seconds, &tm);
#endif
  char out[25];
  std::snprintf(out, sizeof(out), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                static_cast<int>(millis.count()));
  return out;
}

std::string generate_trace_id() {
  thread_local std::mt19937 gen([] {
    std::random_device rd;
    std::seed_seq seed{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
    return std::mt19937(seed);
  }());
  std::uniform_int_distribution<int> dist(0, 255);
  unsigned char bytes[16];
  for (auto& byte : bytes) {
    byte = static_cast<unsigned char>(dist(gen));
  }
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);

  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      out << '-';
    }
    out << std::setw(2) << static_cast<int>(bytes[i]);
  }
  return out.str();
}

}  // namespace auralog
