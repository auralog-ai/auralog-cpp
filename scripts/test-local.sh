#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS="$ROOT/build/_deps"
mkdir -p "$DEPS/nlohmann"

if [[ ! -f "$DEPS/nlohmann/json.hpp" ]]; then
  curl -fsSL \
    https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp \
    -o "$DEPS/nlohmann/json.hpp"
fi

mkdir -p "$ROOT/build/local"
clang++ -std=c++17 -Wall -Wextra -Wpedantic -Werror \
  -I"$ROOT/include" -I"$DEPS" \
  "$ROOT/src/auralog.cpp" \
  "$ROOT/src/curl_transport.cpp" \
  "$ROOT/src/terminate_capture.cpp" \
  "$ROOT/tests/sdk_test.cpp" \
  -lcurl \
  -o "$ROOT/build/local/sdk_test"

"$ROOT/build/local/sdk_test"

