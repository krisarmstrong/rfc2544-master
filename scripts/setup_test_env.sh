#!/bin/bash
# RFC2544 Test Environment Setup
#
# Creates veth pairs for testing and starts reflector
# WebUI accessible from LAN on specified port
#
# Usage: sudo ./setup_test_env.sh [webui_port]

set -e

WEBUI_PORT="${1:-8080}"
VETH_MASTER="veth-master"
VETH_REFLECTOR="veth-reflect"
REFLECTOR_BIN="${REFLECTOR_BIN:-/usr/bin/reflector}"
RFC2544_BIN="${RFC2544_BIN:-./rfc2544-linux}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

info() { echo -e "${GREEN}[INFO]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }

# Check root
if [ "$(id -u)" != "0" ]; then
    error "This script must be run as root"
fi

# Get LAN IP
get_lan_ip() {
    # Try to get IP from common interfaces
    for iface in eth0 en0 ens192 ens33 enp0s3; do
        ip=$(ip -4 addr show "$iface" 2>/dev/null | grep -oP '(?<=inet\s)\d+(\.\d+){3}' | head -1)
        if [ -n "$ip" ]; then
            echo "$ip"
            return
        fi
    done
    # Fallback: get first non-loopback IP
    ip -4 addr show | grep -oP '(?<=inet\s)\d+(\.\d+){3}' | grep -v '^127\.' | head -1
}

LAN_IP=$(get_lan_ip)

echo ""
echo "╔═══════════════════════════════════════════════════════════════╗"
echo "║         RFC2544 Test Environment Setup                        ║"
echo "╠═══════════════════════════════════════════════════════════════╣"
echo "║  This script will:                                            ║"
echo "║  1. Create veth pair for packet testing                       ║"
echo "║  2. Start reflector on one end                                ║"
echo "║  3. Start RFC2544 WebUI accessible from your LAN              ║"
echo "╚═══════════════════════════════════════════════════════════════╝"
echo ""

# Cleanup function
cleanup() {
    info "Cleaning up..."

    # Kill reflector if running
    pkill -f "reflector.*${VETH_REFLECTOR}" 2>/dev/null || true

    # Kill rfc2544 if running
    pkill -f "rfc2544.*--web" 2>/dev/null || true

    # Remove veth pair
    ip link delete "$VETH_MASTER" 2>/dev/null || true

    info "Cleanup complete"
}

# Trap for cleanup on exit
trap cleanup EXIT

# Step 1: Create veth pair
info "Creating veth pair: $VETH_MASTER <-> $VETH_REFLECTOR"

# Remove existing if present
ip link delete "$VETH_MASTER" 2>/dev/null || true

# Create new veth pair
ip link add "$VETH_MASTER" type veth peer name "$VETH_REFLECTOR"

# Bring up both interfaces
ip link set "$VETH_MASTER" up
ip link set "$VETH_REFLECTOR" up

# Disable offloading (important for accurate testing)
for iface in "$VETH_MASTER" "$VETH_REFLECTOR"; do
    ethtool -K "$iface" tx off rx off gso off gro off tso off 2>/dev/null || true
done

# Increase queue length for high-speed testing
ip link set "$VETH_MASTER" txqueuelen 10000
ip link set "$VETH_REFLECTOR" txqueuelen 10000

info "veth pair created and configured"

# Step 2: Start reflector
info "Starting reflector on $VETH_REFLECTOR..."

if [ ! -x "$REFLECTOR_BIN" ]; then
    # Try to find reflector
    for path in ./reflector ../reflector-native/reflector /usr/local/bin/reflector; do
        if [ -x "$path" ]; then
            REFLECTOR_BIN="$path"
            break
        fi
    done
fi

if [ ! -x "$REFLECTOR_BIN" ]; then
    warn "Reflector binary not found. Please set REFLECTOR_BIN environment variable"
    warn "Example: REFLECTOR_BIN=/path/to/reflector $0"
else
    # Start reflector in background
    # Use --no-oui-filter and --no-mac-filter for veth testing (non-NetAlly)
    # For production with NetAlly devices, remove these flags to enable filtering
    "$REFLECTOR_BIN" "$VETH_REFLECTOR" --mode all --no-mac-filter --no-oui-filter --sig all > /var/log/reflector.log 2>&1 &
    REFLECTOR_PID=$!
    sleep 1

    if kill -0 $REFLECTOR_PID 2>/dev/null; then
        info "Reflector started (PID: $REFLECTOR_PID)"
        info "  Filters: MAC=off, OUI=off (for veth testing)"
    else
        error "Failed to start reflector. Check /var/log/reflector.log"
    fi
fi

# Step 3: Start RFC2544 with WebUI
info "Starting RFC2544 Test Master WebUI..."

if [ ! -x "$RFC2544_BIN" ]; then
    # Try to find rfc2544
    for path in ./rfc2544-linux ./rfc2544-v2 /usr/local/bin/rfc2544 /usr/bin/rfc2544; do
        if [ -x "$path" ]; then
            RFC2544_BIN="$path"
            break
        fi
    done
fi

if [ ! -x "$RFC2544_BIN" ]; then
    warn "RFC2544 binary not found. Please set RFC2544_BIN environment variable"
    warn "WebUI will not be started"
else
    # Start with WebUI bound to all interfaces
    "$RFC2544_BIN" -i "$VETH_MASTER" --web ":${WEBUI_PORT}" > /var/log/rfc2544.log 2>&1 &
    RFC2544_PID=$!
    sleep 2

    if kill -0 $RFC2544_PID 2>/dev/null; then
        info "RFC2544 WebUI started (PID: $RFC2544_PID)"
    else
        error "Failed to start RFC2544. Check /var/log/rfc2544.log"
    fi
fi

# Print access info
echo ""
echo "╔═══════════════════════════════════════════════════════════════╗"
echo "║                    Test Environment Ready                      ║"
echo "╠═══════════════════════════════════════════════════════════════╣"
printf "║  %-61s ║\n" "WebUI Access:"
printf "║    %-59s ║\n" "Local:  http://localhost:${WEBUI_PORT}"
if [ -n "$LAN_IP" ]; then
printf "║    %-59s ║\n" "LAN:    http://${LAN_IP}:${WEBUI_PORT}"
fi
echo "╠═══════════════════════════════════════════════════════════════╣"
printf "║  %-61s ║\n" "Test Interface: $VETH_MASTER"
printf "║  %-61s ║\n" "Reflector on:   $VETH_REFLECTOR"
echo "╠═══════════════════════════════════════════════════════════════╣"
echo "║  Quick Test Commands:                                         ║"
echo "║                                                                ║"
printf "║  %-62s║\n" "# Throughput test via CLI:"
printf "║  %-62s║\n" "curl -X POST http://localhost:${WEBUI_PORT}/api/start \\"
printf "║  %-62s║\n" "  -H 'Content-Type: application/json' \\"
printf "║  %-62s║\n" "  -d '{\"interface\":\"${VETH_MASTER}\",\"test_type\":0}'"
echo "║                                                                ║"
printf "║  %-62s║\n" "# Check status:"
printf "║  %-62s║\n" "curl http://localhost:${WEBUI_PORT}/api/stats"
echo "╠═══════════════════════════════════════════════════════════════╣"
echo "║  Press Ctrl+C to stop and cleanup                             ║"
echo "╚═══════════════════════════════════════════════════════════════╝"
echo ""

# Tail logs
info "Tailing logs (Ctrl+C to stop)..."
echo ""

tail -f /var/log/rfc2544.log /var/log/reflector.log 2>/dev/null || {
    # If tail fails, just wait
    while true; do
        sleep 1
    done
}
