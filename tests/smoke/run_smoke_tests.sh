#!/bin/bash
#
# run_smoke_tests.sh - Smoke tests for RFC2544 and Y.1564
#
# Requires: Linux, root/sudo, veth pair support
# Tests all major functionality with short durations
#

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

# Configuration
VETH_MASTER="veth-test-tx"
VETH_REFLECT="veth-test-rx"
IP_MASTER="192.168.254.1"
IP_REFLECT="192.168.254.2"
SUBNET="/24"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="${SCRIPT_DIR}/../.."
RFC2544_BIN="${PROJECT_ROOT}/rfc2544-linux"
REFLECTOR_BIN=""  # Will be set by find_reflector

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0
TESTS_SKIPPED=0

# Logging
log_info()  { echo -e "${CYAN}[INFO]${NC} $1"; }
log_pass()  { echo -e "${GREEN}[PASS]${NC} $1"; }
log_fail()  { echo -e "${RED}[FAIL]${NC} $1"; }
log_skip()  { echo -e "${YELLOW}[SKIP]${NC} $1"; }
log_header() { echo -e "\n${CYAN}=== $1 ===${NC}"; }

# Check if running as root
check_root() {
    if [[ $EUID -ne 0 ]]; then
        echo -e "${RED}Error: Smoke tests require root for veth creation${NC}"
        echo "Usage: sudo $0"
        exit 1
    fi
}

# Find reflector binary
find_reflector() {
    local candidates=(
        "${PROJECT_ROOT}/../reflector-native/reflector-linux"
        "/usr/local/bin/reflector"
        "/usr/bin/reflector"
    )

    for bin in "${candidates[@]}"; do
        if [[ -x "$bin" ]]; then
            REFLECTOR_BIN="$bin"
            return 0
        fi
    done

    log_skip "Reflector not found - some tests will be skipped"
    return 1
}

# Cleanup function
cleanup() {
    log_info "Cleaning up..."

    # Kill any test processes
    pkill -f "reflector.*${VETH_REFLECT}" 2>/dev/null || true
    pkill -f "rfc2544.*${VETH_MASTER}" 2>/dev/null || true

    # Remove veth pair
    ip link delete "${VETH_MASTER}" 2>/dev/null || true

    log_info "Cleanup complete"
}

# Set up trap for cleanup
trap cleanup EXIT

# Create veth pair for testing
setup_veth() {
    log_info "Creating veth pair..."

    # Remove existing if present
    ip link delete "${VETH_MASTER}" 2>/dev/null || true

    # Create veth pair
    ip link add "${VETH_MASTER}" type veth peer name "${VETH_REFLECT}"

    # Configure both ends
    ip addr add "${IP_MASTER}${SUBNET}" dev "${VETH_MASTER}"
    ip addr add "${IP_REFLECT}${SUBNET}" dev "${VETH_REFLECT}"
    ip link set "${VETH_MASTER}" up
    ip link set "${VETH_REFLECT}" up

    # Disable reverse path filtering
    echo 0 > /proc/sys/net/ipv4/conf/${VETH_MASTER}/rp_filter
    echo 0 > /proc/sys/net/ipv4/conf/${VETH_REFLECT}/rp_filter

    # Verify connectivity
    if ping -c 1 -W 1 "${IP_REFLECT}" -I "${VETH_MASTER}" >/dev/null 2>&1; then
        log_info "veth pair ready: ${VETH_MASTER} <-> ${VETH_REFLECT}"
    else
        log_info "veth pair created (ping may not work with raw sockets)"
    fi
}

# Start reflector in background
start_reflector() {
    if [[ -z "$REFLECTOR_BIN" ]]; then
        return 1
    fi

    log_info "Starting reflector on ${VETH_REFLECT}..."

    "${REFLECTOR_BIN}" "${VETH_REFLECT}" \
        --mode all \
        --no-oui-filter \
        --no-mac-filter \
        --sig all \
        >/dev/null 2>&1 &

    REFLECTOR_PID=$!
    sleep 2

    if kill -0 $REFLECTOR_PID 2>/dev/null; then
        log_info "Reflector running (PID: $REFLECTOR_PID)"
        return 0
    else
        log_fail "Reflector failed to start"
        return 1
    fi
}

# Stop reflector
stop_reflector() {
    if [[ -n "$REFLECTOR_PID" ]]; then
        kill $REFLECTOR_PID 2>/dev/null || true
        wait $REFLECTOR_PID 2>/dev/null || true
        REFLECTOR_PID=""
    fi
}

# Run a test and record result
run_test() {
    local name="$1"
    local cmd="$2"
    local expected_exit="${3:-0}"

    TESTS_RUN=$((TESTS_RUN + 1))

    log_info "Running: $name"

    local output
    local exit_code

    set +e
    output=$(eval "$cmd" 2>&1)
    exit_code=$?
    set -e

    if [[ $exit_code -eq $expected_exit ]]; then
        log_pass "$name"
        TESTS_PASSED=$((TESTS_PASSED + 1))
        return 0
    else
        log_fail "$name (exit code: $exit_code, expected: $expected_exit)"
        echo "Output:"
        echo "$output" | head -20
        TESTS_FAILED=$((TESTS_FAILED + 1))
        return 1
    fi
}

# Skip a test
skip_test() {
    local name="$1"
    local reason="$2"

    TESTS_RUN=$((TESTS_RUN + 1))
    TESTS_SKIPPED=$((TESTS_SKIPPED + 1))
    log_skip "$name - $reason"
}

# ============================================================================
# Test Cases
# ============================================================================

test_help() {
    log_header "CLI Help Tests"

    run_test "Help flag (-h)" \
        "${RFC2544_BIN} -h"

    run_test "Help flag (--help)" \
        "${RFC2544_BIN} --help"
}

test_binary_exists() {
    log_header "Binary Check"

    if [[ ! -x "${RFC2544_BIN}" ]]; then
        log_fail "Binary not found: ${RFC2544_BIN}"
        log_info "Building binary..."
        (cd "${PROJECT_ROOT}" && make linux)
    fi

    run_test "Binary is executable" \
        "test -x ${RFC2544_BIN}"
}

test_throughput() {
    log_header "RFC2544 Throughput Test (Section 26.1)"

    if [[ -z "$REFLECTOR_PID" ]]; then
        skip_test "Throughput test" "Reflector not running"
        return
    fi

    run_test "Throughput test (2s trial, 128 bytes)" \
        "${RFC2544_BIN} ${VETH_MASTER} --test throughput --duration 2 --size 128 --force-packet --resolution 5"

    run_test "Throughput test (2s trial, 1518 bytes)" \
        "${RFC2544_BIN} ${VETH_MASTER} --test throughput --duration 2 --size 1518 --force-packet --resolution 5"
}

test_latency() {
    log_header "RFC2544 Latency Test (Section 26.2)"

    if [[ -z "$REFLECTOR_PID" ]]; then
        skip_test "Latency test" "Reflector not running"
        return
    fi

    run_test "Latency test (3s duration, 64 bytes)" \
        "${RFC2544_BIN} ${VETH_MASTER} --test latency --duration 3 --size 64 --force-packet"

    run_test "Latency test (3s duration, 512 bytes)" \
        "${RFC2544_BIN} ${VETH_MASTER} --test latency --duration 3 --size 512 --force-packet"
}

test_frame_loss() {
    log_header "RFC2544 Frame Loss Test (Section 26.3)"

    if [[ -z "$REFLECTOR_PID" ]]; then
        skip_test "Frame loss test" "Reflector not running"
        return
    fi

    run_test "Frame loss test (2s duration, 256 bytes)" \
        "${RFC2544_BIN} ${VETH_MASTER} --test loss --duration 2 --size 256 --force-packet"
}

test_back_to_back() {
    log_header "RFC2544 Back-to-Back Test (Section 26.4)"

    if [[ -z "$REFLECTOR_PID" ]]; then
        skip_test "Back-to-back test" "Reflector not running"
        return
    fi

    run_test "Back-to-back burst test (2s duration, 512 bytes)" \
        "${RFC2544_BIN} ${VETH_MASTER} --test burst --duration 2 --size 512 --force-packet"
}

test_y1564_config() {
    log_header "Y.1564 Service Configuration Test"

    if [[ -z "$REFLECTOR_PID" ]]; then
        skip_test "Y.1564 config test" "Reflector not running"
        return
    fi

    # Check if Y.1564 is supported
    if ! ${RFC2544_BIN} --help 2>&1 | grep -q "y1564"; then
        skip_test "Y.1564 config test" "Y.1564 not supported in this build"
        return
    fi

    run_test "Y.1564 config test (CIR=100Mbps, 60s steps)" \
        "${RFC2544_BIN} ${VETH_MASTER} --test y1564_config --cir 100 --step-duration 5 --force-packet"
}

test_y1564_perf() {
    log_header "Y.1564 Service Performance Test"

    if [[ -z "$REFLECTOR_PID" ]]; then
        skip_test "Y.1564 perf test" "Reflector not running"
        return
    fi

    # Check if Y.1564 is supported
    if ! ${RFC2544_BIN} --help 2>&1 | grep -q "y1564"; then
        skip_test "Y.1564 perf test" "Y.1564 not supported in this build"
        return
    fi

    run_test "Y.1564 perf test (30s duration)" \
        "${RFC2544_BIN} ${VETH_MASTER} --test y1564_perf --cir 100 --duration 30 --force-packet"
}

test_json_output() {
    log_header "JSON Output Tests"

    if [[ -z "$REFLECTOR_PID" ]]; then
        skip_test "JSON output test" "Reflector not running"
        return
    fi

    run_test "Latency test with JSON output" \
        "${RFC2544_BIN} ${VETH_MASTER} --test latency --duration 2 --size 128 --json --force-packet | head -1 | grep -q '{'"
}

test_csv_output() {
    log_header "CSV Output Tests"

    if [[ -z "$REFLECTOR_PID" ]]; then
        skip_test "CSV output test" "Reflector not running"
        return
    fi

    run_test "Latency test with CSV output" \
        "${RFC2544_BIN} ${VETH_MASTER} --test latency --duration 2 --size 128 --csv --force-packet | head -1 | grep -q ','"
}

# ============================================================================
# Main
# ============================================================================

main() {
    echo -e "${CYAN}╔════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${CYAN}║         RFC 2544 / Y.1564 Smoke Test Suite                 ║${NC}"
    echo -e "${CYAN}╚════════════════════════════════════════════════════════════╝${NC}"
    echo ""

    check_root
    test_binary_exists
    find_reflector
    setup_veth

    # Start reflector if available
    if [[ -n "$REFLECTOR_BIN" ]]; then
        start_reflector || true
    fi

    # Run test suites
    test_help
    test_throughput
    test_latency
    test_frame_loss
    test_back_to_back
    test_y1564_config
    test_y1564_perf
    test_json_output
    test_csv_output

    # Stop reflector
    stop_reflector

    # Summary
    echo ""
    log_header "Test Summary"
    echo -e "  Total:   ${TESTS_RUN}"
    echo -e "  ${GREEN}Passed:${NC}  ${TESTS_PASSED}"
    echo -e "  ${RED}Failed:${NC}  ${TESTS_FAILED}"
    echo -e "  ${YELLOW}Skipped:${NC} ${TESTS_SKIPPED}"
    echo ""

    if [[ $TESTS_FAILED -gt 0 ]]; then
        echo -e "${RED}SMOKE TESTS FAILED${NC}"
        exit 1
    else
        echo -e "${GREEN}ALL SMOKE TESTS PASSED${NC}"
        exit 0
    fi
}

main "$@"
