#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS="$ROOT/build/_deps"
JSON_SHA256="aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63"
mkdir -p "$DEPS/nlohmann"

if [[ ! -f "$DEPS/nlohmann/json.hpp" ]]; then
  curl -fsSL \
    https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp \
    -o "$DEPS/nlohmann/json.hpp"
fi
actual_sha256="$(shasum -a 256 "$DEPS/nlohmann/json.hpp" | awk '{print $1}')"
if [[ "$actual_sha256" != "$JSON_SHA256" ]]; then
  echo "nlohmann/json.hpp SHA256 mismatch" >&2
  exit 1
fi

mkdir -p "$ROOT/build/local"
clang++ -std=c++17 -Wall -Wextra -Wpedantic -Werror \
  -I"$ROOT/include" -I"$DEPS" \
  "$ROOT/src/auralogs.cpp" \
  "$ROOT/src/curl_transport.cpp" \
  "$ROOT/src/terminate_capture.cpp" \
  "$ROOT/tests/sdk_test.cpp" \
  -lcurl \
  -o "$ROOT/build/local/sdk_test"

"$ROOT/build/local/sdk_test"
