#include <atomic>
#include <auralog/auralog.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

#define CHECK(expr)                                                                      \
  do {                                                                                   \
    if (!(expr)) {                                                                       \
      std::cerr << "CHECK failed: " #expr " at " << __FILE__ << ':' << __LINE__ << '\n'; \
      std::exit(1);                                                                      \
    }                                                                                    \
  } while (false)

class RecordingTransport final : public auralog::Transport {
 public:
  explicit RecordingTransport(std::vector<auralog::SendResult> results = {})
      : results_(std::move(results)) {}

  auralog::SendResult send_batch(const std::vector<auralog::LogEntry>& entries) override {
    std::lock_guard<std::mutex> lock(mutex_);
    batches.push_back(entries);
    return next_result();
  }

  auralog::SendResult send_single(const auralog::LogEntry& entry) override {
    std::lock_guard<std::mutex> lock(mutex_);
    singles.push_back(entry);
    return next_result();
  }

  std::vector<std::vector<auralog::LogEntry>> batches;
  std::vector<auralog::LogEntry> singles;

 private:
  auralog::SendResult next_result() {
    if (result_index_ >= results_.size()) {
      return auralog::SendResult::Success;
    }
    return results_[result_index_++];
  }

  std::mutex mutex_;
  std::vector<auralog::SendResult> results_;
  std::size_t result_index_ = 0;
};

class BlockingTransport final : public auralog::Transport {
 public:
  auralog::SendResult send_batch(const std::vector<auralog::LogEntry>& entries) override {
    std::lock_guard<std::mutex> lock(mutex_);
    batches.push_back(entries);
    return auralog::SendResult::Success;
  }

  auralog::SendResult send_single(const auralog::LogEntry& entry) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      singles.push_back(entry);
      entered = true;
    }
    cv.notify_all();

    std::unique_lock<std::mutex> lock(mutex_);
    cv.wait(lock, [this] { return release; });
    return auralog::SendResult::Success;
  }

  void wait_until_entered() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv.wait(lock, [this] { return entered; });
  }

  void release_send() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      release = true;
    }
    cv.notify_all();
  }

  std::vector<std::vector<auralog::LogEntry>> batches;
  std::vector<auralog::LogEntry> singles;

 private:
  std::mutex mutex_;
  std::condition_variable cv;
  bool entered = false;
  bool release = false;
};

auralog::Config config() {
  auralog::Config cfg;
  cfg.api_key = "aura_test";
  cfg.environment = "test";
  cfg.flush_interval = 1h;
  cfg.retry_initial_delay = 1ms;
  cfg.retry_max_delay = 1ms;
  cfg.shutdown_timeout = 100ms;
  return cfg;
}

void test_wire_format() {
  auto transport = std::make_shared<RecordingTransport>();
  auto client = auralog::Client::create(config(), transport);
  client->set_global_metadata({{{"service", "checkout"}}});
  client->info("started", {{"order_id", "ord_1"}});
  client->error("failed", {{"reason", "declined"}});
  client->flush();

  CHECK(transport->batches.size() == 1);
  CHECK(transport->singles.size() == 1);
  auto wire = transport->batches[0][0].to_wire();
  CHECK(wire["level"] == "info");
  CHECK(wire["environment"] == "test");
  CHECK(wire["metadata"]["service"] == "checkout");
  CHECK(wire["metadata"]["order_id"] == "ord_1");
  CHECK(wire["timestamp"].get<std::string>().back() == 'Z');
  client->shutdown();
}

void test_flush_drains_all_batches() {
  auto transport = std::make_shared<RecordingTransport>();
  auto cfg = config();
  cfg.max_batch_size = 50;
  auto client = auralog::Client::create(cfg, transport);
  for (int i = 0; i < 120; ++i) {
    client->info("bulk", {{"index", i}});
  }
  client->flush();
  std::size_t total = 0;
  for (const auto& batch : transport->batches) {
    total += batch.size();
  }
  CHECK(transport->batches.size() == 3);
  CHECK(total == 120);
  client->shutdown();
}

void test_flush_waits_for_in_flight_send() {
  auto transport = std::make_shared<BlockingTransport>();
  auto cfg = config();
  cfg.shutdown_timeout = 1s;
  auto client = auralog::Client::create(cfg, transport);
  client->error("in flight", {});
  transport->wait_until_entered();

  std::atomic<bool> flushed{false};
  std::thread flushing([&] {
    client->flush();
    flushed = true;
  });
  std::this_thread::sleep_for(20ms);
  CHECK(!flushed.load());
  transport->release_send();
  flushing.join();
  CHECK(flushed.load());
  CHECK(transport->singles.size() == 1);
  client->shutdown();
}

void test_retry_and_permanent_failure() {
  auto retrying = std::make_shared<RecordingTransport>(std::vector<auralog::SendResult>{
      auralog::SendResult::RetryableFailure, auralog::SendResult::Success});
  auto client = auralog::Client::create(config(), retrying);
  client->info("retry", {});
  client->flush();
  CHECK(retrying->batches.size() == 2);
  client->shutdown();

  auto permanent = std::make_shared<RecordingTransport>(
      std::vector<auralog::SendResult>{auralog::SendResult::PermanentFailure});
  auto client2 = auralog::Client::create(config(), permanent);
  client2->error("bad auth", {});
  client2->flush();
  CHECK(permanent->singles.size() == 1);
  client2->shutdown();
}

void test_queue_trim_and_scalar_metadata() {
  auto transport = std::make_shared<RecordingTransport>();
  auto cfg = config();
  cfg.max_queue_size = 2;
  auto client = auralog::Client::create(cfg, transport);
  for (int i = 0; i < 5; ++i) {
    client->info("trim", {{"index", i}});
  }
  client->flush();
  CHECK(transport->batches[0].size() == 2);
  CHECK(transport->batches[0][0].metadata.value()["index"] == 3);
  CHECK(transport->batches[0][1].metadata.value()["index"] == 4);

  auto transport2 = std::make_shared<RecordingTransport>();
  auto client2 = auralog::Client::create(config(), transport2);
  client2->info("scalar", "hello");
  client2->flush();
  CHECK(transport2->batches[0][0].metadata.value()["value"] == "hello");
  client->shutdown();
  client2->shutdown();
}

class AlwaysRetryableTransport final : public auralog::Transport {
 public:
  auralog::SendResult send_batch(const std::vector<auralog::LogEntry>& entries) override {
    std::lock_guard<std::mutex> lock(mutex_);
    batch_attempts += entries.size();
    return auralog::SendResult::RetryableFailure;
  }

  auralog::SendResult send_single(const auralog::LogEntry&) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++single_attempts;
    return auralog::SendResult::RetryableFailure;
  }

  std::size_t batch_attempts = 0;
  std::size_t single_attempts = 0;

 private:
  std::mutex mutex_;
};

void test_retry_exhaustion_drops_entries() {
  auto transport = std::make_shared<AlwaysRetryableTransport>();
  auto cfg = config();
  cfg.max_retry_attempts = 3;
  cfg.retry_initial_delay = 1ms;
  cfg.retry_max_delay = 1ms;
  auto client = auralog::Client::create(cfg, transport);
  client->info("doomed", {{"index", 0}});
  client->flush_for(1s);

  CHECK(transport->batch_attempts == 3);
  CHECK(transport->single_attempts == 0);
  client->shutdown();
}

void test_supplier_exception_does_not_crash_logging() {
  auto transport = std::make_shared<RecordingTransport>();
  auto client = auralog::Client::create(config(), transport);
  client->set_global_metadata_supplier([] {
    throw std::runtime_error("supplier exploded");
    return nlohmann::json::object();
  });
  client->info("survives", {{"order_id", "ord_1"}});
  client->flush();

  CHECK(transport->batches.size() == 1);
  CHECK(transport->batches[0][0].metadata.value()["order_id"] == "ord_1");
  client->shutdown();
}

void test_rejects_insecure_endpoint_by_default() {
  auralog::Config insecure;
  insecure.api_key = "k";
  insecure.endpoint = "http://insecure";
  bool threw = false;
  try {
    (void)auralog::Client::create(insecure, std::make_shared<RecordingTransport>());
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  auralog::Config opt_out;
  opt_out.api_key = "k";
  opt_out.endpoint = "http://insecure";
  opt_out.allow_insecure_endpoint = true;
  opt_out.flush_interval = 1h;
  opt_out.retry_initial_delay = 1ms;
  opt_out.retry_max_delay = 1ms;
  opt_out.shutdown_timeout = 100ms;
  auto client = auralog::Client::create(opt_out, std::make_shared<RecordingTransport>());
  CHECK(client != nullptr);
  client->shutdown();
}

void test_runtime_updates_and_validation() {
  auto transport = std::make_shared<RecordingTransport>();
  auto cfg = config();
  cfg.max_batch_size = 1;
  cfg.trace_id = "trace-one";
  auto client = auralog::Client::create(cfg, transport);
  client->set_global_metadata({{{"service", "one"}}});
  client->info("first", {});
  client->set_trace_id("trace-two");
  client->set_global_metadata({{{"service", "two"}}});
  client->info("second", {});
  client->flush();
  CHECK(transport->batches[0][0].trace_id == "trace-one");
  CHECK(transport->batches[0][0].metadata.value()["service"] == "one");
  CHECK(transport->batches[1][0].trace_id == "trace-two");
  CHECK(transport->batches[1][0].metadata.value()["service"] == "two");
  client->shutdown();

  auto bad = config();
  bad.flush_interval = 0ms;
  bool threw = false;
  try {
    (void)auralog::Client::create(bad, std::make_shared<RecordingTransport>());
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

}  // namespace

int main() {
  test_wire_format();
  test_flush_drains_all_batches();
  test_flush_waits_for_in_flight_send();
  test_retry_and_permanent_failure();
  test_retry_exhaustion_drops_entries();
  test_supplier_exception_does_not_crash_logging();
  test_queue_trim_and_scalar_metadata();
  test_rejects_insecure_endpoint_by_default();
  test_runtime_updates_and_validation();
  return 0;
}
