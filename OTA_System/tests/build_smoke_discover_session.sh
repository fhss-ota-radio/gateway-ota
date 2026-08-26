#!/usr/bin/env bash
set -euo pipefail

# Build the Qt-free real-radio DISCOVER -> targeted OTA CLI on Raspberry Pi.
# This script only compiles; it never opens /dev/cc1101 or transmits RF.

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
project_dir="$(cd -- "$script_dir/.." && pwd -P)"
gateway_dir="$(cd -- "$project_dir/.." && pwd -P)"
protocol_include="$(cd -- "$gateway_dir/../ota-protocol/include" && pwd -P)"
build_dir="$project_dir/build"
output="$build_dir/ota_smoke_discover_session_send"

command -v g++ >/dev/null 2>&1 || {
    echo "BLOCKED: g++ is unavailable" >&2
    exit 2
}
[[ -f "$protocol_include/ota_protocol.h" ]] || {
    echo "BLOCKED: missing $protocol_include/ota_protocol.h" >&2
    exit 2
}

mkdir -p "$build_dir"
g++ -std=c++17 -O2 -Wall -Wextra -pthread \
    -I"$project_dir" \
    -I"$project_dir/core" \
    -I"$project_dir/session" \
    -I"$project_dir/transport" \
    -I"$protocol_include" \
    "$project_dir/tests/smoke_discover_session_send_main.cpp" \
    "$project_dir/core/binsplitter.cpp" \
    "$project_dir/core/sha256.cpp" \
    "$project_dir/session/discovery.cpp" \
    "$project_dir/session/otasession.cpp" \
    "$project_dir/session/simplereceiver.cpp" \
    "$project_dir/session/simplesender.cpp" \
    "$project_dir/transport/cc1101transport.cpp" \
    -o "$output"

sha256sum "$output"
echo "Built: $output"
