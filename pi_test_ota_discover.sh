#!/usr/bin/env bash
set -euo pipefail

# Run this script on the Raspberry Pi. It never uses sudo, reloads a module,
# changes device permissions, or starts more than one RF process.

usage() {
    cat <<'EOF'
Usage: pi_test_ota_discover.sh <positive|negative> [gateway_dir] [device] [wait_ms]

  positive  ESP32 is currently in MENU_OTA; at least one device must be found.
  negative  ESP32 is outside MENU_OTA; no device must be found.

Defaults:
  gateway_dir: $HOME/FHSS/gateway-ota
  device:      /dev/cc1101
  wait_ms:     1000
EOF
}

mode=${1:-}
gateway_dir=${2:-"$HOME/FHSS/gateway-ota"}
device=${3:-/dev/cc1101}
wait_ms=${4:-1000}

if [[ $mode != positive && $mode != negative ]]; then
    usage >&2
    exit 64
fi
if [[ ! $wait_ms =~ ^[1-9][0-9]*$ ]] || (( wait_ms > 5000 )); then
    echo "FAIL: wait_ms must be an integer from 1 through 5000" >&2
    exit 64
fi

project_dir="$gateway_dir/OTA_System"
build_dir="$project_dir/build"

[[ -f "$project_dir/CMakeLists.txt" ]] || {
    echo "BLOCKED: missing $project_dir/CMakeLists.txt" >&2
    exit 2
}
[[ -e "$device" ]] || {
    echo "BLOCKED: CC1101 device does not exist: $device" >&2
    exit 2
}
[[ -r "$device" && -w "$device" ]] || {
    echo "BLOCKED: current user needs read/write access to $device" >&2
    ls -l "$device" >&2 || true
    exit 2
}
command -v g++ >/dev/null 2>&1 || {
    echo "BLOCKED: g++ is unavailable" >&2
    exit 2
}
command -v timeout >/dev/null 2>&1 || {
    echo "BLOCKED: timeout is unavailable" >&2
    exit 2
}

# Refuse to compete with another process for the single CC1101 radio.
if command -v fuser >/dev/null 2>&1; then
    owners=$(fuser "$device" 2>/dev/null || true)
    if [[ -n $owners ]]; then
        echo "BLOCKED: another process owns $device (PID:$owners)" >&2
        exit 2
    fi
fi

run_id=$(date -u +%Y%m%dT%H%M%SZ)
artifact_dir="$gateway_dir/artifacts/$run_id"
log_dir="$artifact_dir/logs"
mkdir -p "$log_dir"
build_log="$log_dir/build-ota-smoke-discover.log"
rf_log="$log_dir/ota-smoke-discover-$mode.log"

echo "[preflight] mode=$mode gateway=$gateway_dir device=$device wait_ms=$wait_ms"
echo "[preflight] evidence=$artifact_dir"

# The top-level CMake project requires Qt for its GUI before it defines this
# non-Qt CLI. Build only the four standard-C++ sources needed by DISCOVER so the
# Raspberry Pi does not need Qt merely for an RF smoke test.
protocol_include="$gateway_dir/../ota-protocol/include"
[[ -f "$protocol_include/ota_protocol.h" ]] || {
    echo "BLOCKED: missing $protocol_include/ota_protocol.h" >&2
    exit 2
}
mkdir -p "$build_dir"
binary="$build_dir/ota_smoke_discover"
g++ -std=c++17 -O2 -Wall -Wextra -pthread \
    -I"$project_dir" \
    -I"$project_dir/session" \
    -I"$project_dir/transport" \
    -I"$protocol_include" \
    "$project_dir/tests/smoke_discover_main.cpp" \
    "$project_dir/session/discovery.cpp" \
    "$project_dir/session/simplereceiver.cpp" \
    "$project_dir/transport/cc1101transport.cpp" \
    -o "$binary" 2>&1 | tee "$build_log"

[[ -x "$binary" ]] || {
    echo "BLOCKED: build did not produce $binary" >&2
    exit 2
}

# The CLI transmits exactly one DISCOVER. The outer timeout also bounds startup,
# driver calls, the receive window, and shutdown.
outer_timeout=$(( (wait_ms + 6999) / 1000 ))
set +e
timeout --signal=TERM "${outer_timeout}s" \
    "$binary" "$device" "$wait_ms" 2>&1 | tee "$rf_log"
pipeline_status=("${PIPESTATUS[@]}")
set -e
cli_status=${pipeline_status[0]}

if (( cli_status == 124 )); then
    echo "FAIL: ota_smoke_discover exceeded ${outer_timeout}s" >&2
    exit 1
fi
if (( cli_status != 0 )); then
    echo "FAIL: ota_smoke_discover exited $cli_status" >&2
    exit 1
fi

found_count=$(grep -c 'device_id=' "$rf_log" || true)
if [[ $mode == positive ]]; then
    if (( found_count < 1 )); then
        echo "FAIL: MENU_OTA positive test found no ESP32" >&2
        exit 1
    fi
    echo "PASS: discovered $found_count ESP32 device(s) in MENU_OTA"
else
    if (( found_count != 0 )); then
        echo "FAIL: ESP32 responded outside MENU_OTA ($found_count device(s))" >&2
        exit 1
    fi
    grep -F '응답 없음' "$rf_log" >/dev/null || {
        echo "FAIL: negative run did not produce the expected no-response result" >&2
        exit 1
    }
    echo "PASS: no ESP32 responded outside MENU_OTA"
fi

sha256sum "$build_log" "$rf_log" | tee "$artifact_dir/SHA256SUMS"
echo "Evidence: $artifact_dir"
