# auralogs-cpp (Beta)

C++ SDK for [Auralogs](https://auralogs.ai) — agentic logging and application awareness.

Auralogs uses Claude as an on-call engineer: it monitors your logs and errors, alerts you when something's wrong, and opens fix PRs automatically.

[![license](https://img.shields.io/badge/license-MIT-blue.svg)](./LICENSE)

## Requirements

- C++17 or later
- CMake 3.20+
- libcurl
- nlohmann/json, fetched by CMake

## Quick Start

```cpp
#include <auralogs/auralogs.hpp>

int main() {
  auralogs::Config config;
  const char* api_key = std::getenv("AURALOG_API_KEY");
  config.api_key = api_key != nullptr ? api_key : "aura_your_key";
  config.environment = "production";

  auto client = auralogs::init(config);
  client->info("user signed in", {{"user_id", "123"}});
  client->error("payment failed", {{"order_id", "abc"}});
  client->shutdown();
}
```

## Configuration

| Option | Default | Description |
|---|---:|---|
| `api_key` | required | Auralogs project API key |
| `environment` | `production` | Environment label |
| `endpoint` | `https://ingest.auralogs.ai` | Ingest endpoint |
| `flush_interval` | `5000ms` | Time between automatic background flushes |
| `max_batch_size` | `50` | Maximum entries per `/v1/logs` request |
| `max_queue_size` | `1000` | Maximum pending entries before dropping oldest |
| `max_retry_attempts` | `5` | Retry cap for retryable delivery failures |
| `retry_initial_delay` | `1000ms` | Initial retry delay |
| `retry_max_delay` | `30000ms` | Maximum retry delay |
| `http_timeout` | `30000ms` | libcurl connect/total request timeout |
| `shutdown_timeout` | `2000ms` | Default budget for `flush()`, `shutdown()`, and destructor cleanup |
| `trace_id` | generated | Trace ID attached to every log |
| `global_metadata` | none | Static metadata merged into every log |
| `global_metadata_supplier` | none | Callable metadata supplier, invoked per log |
| `capture_terminate` | `false` | Install `std::set_terminate` capture |
| `allow_insecure_endpoint` | `false` | Permit non-`https://` endpoints. Off by default — `Client::create` throws `std::invalid_argument` for plaintext endpoints unless this is set |

## Transport Semantics

- `debug`, `info`, and `warn` logs batch to `/v1/logs`.
- `error` and `fatal` logs are prioritized to `/v1/logs/single`.
- `flush()` drains queued and in-flight logs synchronously until `shutdown_timeout`; use
  `flush_for(timeout)` to choose a different budget.
- 4xx ingest responses are permanent failures and are not retried.
- 5xx and network failures retry up to `max_retry_attempts`.
- `shutdown()` waits for the worker and then drains pending queues using `shutdown_timeout`.
- libcurl connect/request timeout bounds in-flight network work. If shutdown starts while the
  worker is inside libcurl, elapsed shutdown time can include up to one `http_timeout`.
- Destructor cleanup is best-effort; call `shutdown()` in short-lived programs.
- `auralogs::init(config)` throws if a global client is already initialized.
- The project API key is sent in the JSON body as `projectApiKey`, matching the other Auralogs
  SDKs and ingest wire format.

## Metadata

Metadata is `nlohmann::json`. Object metadata merges directly; scalar or array metadata is wrapped as `{ "value": ... }` so values are not silently discarded.

```cpp
client->set_global_metadata({{"service", "checkout"}});
client->set_global_metadata_supplier([] {
  return nlohmann::json{{"tenant", current_tenant()}};
});
```

Supplier exceptions are caught and self-logged once to `std::cerr`.

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

`CMAKE_EXPORT_COMPILE_COMMANDS` is enabled by default for clangd and other IDEs.

This repository also includes `scripts/test-local.sh`, which compiles with `clang++` directly for environments where CMake is unavailable.

The CMake target builds a static library by default. Windows DLL consumers should add symbol
visibility/export annotations in their integration until a shared-library package is published.

## Documentation

Full docs at [docs.auralogs.ai](https://docs.auralogs.ai).

## Security

Found a vulnerability? See [SECURITY.md](./SECURITY.md) for how to report it.

## License

[MIT](./LICENSE) © James Thomas
