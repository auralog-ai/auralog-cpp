#include <auralogs/auralogs.hpp>
#include <cstdlib>
#include <iostream>

int main() {
  auralogs::Config config;
  const char* api_key = std::getenv("AURALOG_API_KEY");
  config.api_key = api_key != nullptr ? api_key : "aura_your_key";
  config.environment = "production";

  auto client = auralogs::init(config);
  client->info("user signed in", {{"user_id", "123"}});
  client->error("payment failed", {{"order_id", "abc"}});
  client->shutdown();

  std::cout << "sent example logs\n";
  return 0;
}
