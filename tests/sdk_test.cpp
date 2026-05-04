#include <auralog/auralog.hpp>

#include <cassert>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <vector>

using namespace std::chrono_literals;

namespace {

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

  assert(transport->batches.size() == 1);
  assert(transport->singles.size() == 1);
  auto wire = transport->batches[0][0].to_wire();
  assert(wire["level"] == "info");
  assert(wire["environment"] == "test");
  assert(wire["metadata"]["service"] == "checkout");
  assert(wire["metadata"]["order_id"] == "ord_1");
  assert(wire["timestamp"].get<std::string>().back() == 'Z');
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
  assert(transport->batches.size() == 3);
  assert(total == 120);
  client->shutdown();
}

void test_retry_and_permanent_failure() {
  auto retrying = std::make_shared<RecordingTransport>(
      std::vector<auralog::SendResult>{auralog::SendResult::RetryableFailure,
                                       auralog::SendResult::Success});
  auto client = auralog::Client::create(config(), retrying);
  client->info("retry", {});
  client->flush();
  assert(retrying->batches.size() == 2);
  client->shutdown();

  auto permanent = std::make_shared<RecordingTransport>(
      std::vector<auralog::SendResult>{auralog::SendResult::PermanentFailure});
  auto client2 = auralog::Client::create(config(), permanent);
  client2->error("bad auth", {});
  client2->flush();
  assert(permanent->singles.size() == 1);
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
  assert(transport->batches[0].size() == 2);
  assert(transport->batches[0][0].metadata.value()["index"] == 3);
  assert(transport->batches[0][1].metadata.value()["index"] == 4);

  auto transport2 = std::make_shared<RecordingTransport>();
  auto client2 = auralog::Client::create(config(), transport2);
  client2->info("scalar", "hello");
  client2->flush();
  assert(transport2->batches[0][0].metadata.value()["value"] == "hello");
  client->shutdown();
  client2->shutdown();
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
  assert(transport->batches[0][0].trace_id == "trace-one");
  assert(transport->batches[0][0].metadata.value()["service"] == "one");
  assert(transport->batches[1][0].trace_id == "trace-two");
  assert(transport->batches[1][0].metadata.value()["service"] == "two");
  client->shutdown();

  auto bad = config();
  bad.flush_interval = 0ms;
  bool threw = false;
  try {
    (void)auralog::Client::create(bad, std::make_shared<RecordingTransport>());
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  assert(threw);
}

}  // namespace

int main() {
  test_wire_format();
  test_flush_drains_all_batches();
  test_retry_and_permanent_failure();
  test_queue_trim_and_scalar_metadata();
  test_runtime_updates_and_validation();
  return 0;
}

