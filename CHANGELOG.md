# Changelog

All notable changes to `auralog-cpp` are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0] - 2026-05-04

### Added

- Initial beta C++17 SDK.
- Manual logging API with `debug`, `info`, `warn`, `error`, and `fatal`.
- Background worker with batching, single-error priority queue, deterministic `flush`, and bounded `shutdown`.
- libcurl transport with request timeout and 4xx/5xx failure classification.
- `nlohmann::json` metadata with global metadata and supplier support.
- Optional `std::set_terminate` capture.
- CMake package target and local clang test script.
- Project API key is intentionally sent in the request body as `projectApiKey`, matching the
  Auralog ingest wire format used by the other SDKs.
