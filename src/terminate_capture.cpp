#include <auralog/auralog.hpp>

#include <exception>
#include <cstdlib>
#include <memory>
#include <mutex>

namespace auralog {
namespace {

std::mutex terminate_mutex;
std::weak_ptr<Client> terminate_client;
std::terminate_handler previous_handler = nullptr;

void auralog_terminate_handler() {
  if (auto client = terminate_client.lock()) {
    client->fatal("process terminated", {{"source", "cpp_terminate"}});
    client->flush();
  }
  if (previous_handler != nullptr) {
    previous_handler();
  }
  std::abort();
}

}  // namespace

void install_terminate_capture(std::shared_ptr<Client> client) {
  std::lock_guard<std::mutex> lock(terminate_mutex);
  terminate_client = std::move(client);
  if (previous_handler == nullptr) {
    previous_handler = std::set_terminate(auralog_terminate_handler);
  }
}

}  // namespace auralog
