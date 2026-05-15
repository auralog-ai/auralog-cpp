# Changelog

All notable changes to `auralogs-cpp` are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-05-15

### Changed

- **BREAKING: Renamed CMake project** `auralog-cpp` → `auralogs-cpp`.
- **BREAKING: Renamed include path** `auralog/auralog.hpp` → `auralogs/auralogs.hpp`. Update includes:
  ```diff
  - #include <auralog/auralog.hpp>
  + #include <auralogs/auralogs.hpp>
  ```
- Default ingest endpoint updated to `https://ingest.auralogs.ai`.
- Repository moved to https://github.com/auralogs-ai/auralogs-cpp.

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
  Auralogs ingest wire format used by the other SDKs.
