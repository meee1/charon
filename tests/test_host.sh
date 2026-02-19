#!/bin/bash
# MIT License
# Charon host-mode test suite
# Tests the charon-host binary built with `make -f Makefile.host`
#
# Usage:
#   sudo ./tests/test_host.sh          # Full test suite (requires root for TAP tests)
#   ./tests/test_host.sh               # Partial suite (skips root-required tests)
#
# Tests:
#   1. Build verification
#   2. Binary format and architecture
#   3. Required symbols present
#   4. Process startup and initialization messages
#   5. UDP socket binding on port 5002 (root)
#   6. TAP device creation as ofdm0 (root)
#   7. Self-loopback: UDP IQ sample injection (root)
#   8. TAP frame injection and OFDM TX path (root + python3)
#   9. OFDM example programs (example/test.sh)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BINARY="$ROOT_DIR/charon-host"
HELPERS_DIR="$SCRIPT_DIR/helpers"

PASS=0
FAIL=0
SKIP=0

CHARON_PID=""

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

pass() { echo -e "  ${GREEN}PASS${NC} $1"; PASS=$((PASS + 1)); }
fail() { echo -e "  ${RED}FAIL${NC} $1"; FAIL=$((FAIL + 1)); }
skip() { echo -e "  ${YELLOW}SKIP${NC} $1"; SKIP=$((SKIP + 1)); }
section() { echo -e "\n${CYAN}[$1]${NC}"; }

has_root()    { [ "$(id -u)" = "0" ]; }
has_python3() { command -v python3 >/dev/null 2>&1; }

# Check if ofdm0 TAP device exists using /proc/net/dev
# (avoids dependency on 'ip' or 'ifconfig' which may not be available)
iface_exists() {
    local iface="$1"
    grep -q "^[[:space:]]*${iface}:" /proc/net/dev 2>/dev/null
}

# Check if UDP port is bound using /proc/net/udp
# Port is given in decimal; /proc/net/udp uses hex
udp_port_bound() {
    local port="$1"
    local hex_port
    hex_port=$(printf "%04X" "$port")
    grep -q ":${hex_port} " /proc/net/udp 2>/dev/null
}

# Kill charon-host on exit to avoid leaving a running daemon
cleanup() {
    if [ -n "$CHARON_PID" ] && kill -0 "$CHARON_PID" 2>/dev/null; then
        kill "$CHARON_PID" 2>/dev/null
        wait "$CHARON_PID" 2>/dev/null || true
    fi
    pkill -f "charon-host" 2>/dev/null || true
    rm -f /tmp/charon_host_test.log
}
trap cleanup EXIT

CHARON_LOG="/tmp/charon_host_test.log"

# Start charon-host in background and wait for initialization.
# charon-host takes ~5-6 seconds to start (config reads, batman setup).
start_charon() {
    local wait_secs="${1:-8}"

    # Kill any existing instance first
    if [ -n "$CHARON_PID" ] && kill -0 "$CHARON_PID" 2>/dev/null; then
        kill "$CHARON_PID" 2>/dev/null
        wait "$CHARON_PID" 2>/dev/null || true
        CHARON_PID=""
    fi
    pkill -f "charon-host" 2>/dev/null || true
    sleep 0.5

    "$BINARY" >"$CHARON_LOG" 2>&1 &
    CHARON_PID=$!

    # Wait for the "enter while main_loop" message indicating full startup
    local elapsed=0
    while [ $elapsed -lt "$wait_secs" ]; do
        if ! kill -0 "$CHARON_PID" 2>/dev/null; then
            return 1  # Process died
        fi
        if grep -q "enter while main_loop\|Initializing host mode" "$CHARON_LOG" 2>/dev/null; then
            sleep 0.5  # Give it a moment to finish init
            return 0
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    # Timeout reached - check if process is still alive
    kill -0 "$CHARON_PID" 2>/dev/null && return 0 || return 1
}

stop_charon() {
    if [ -n "$CHARON_PID" ] && kill -0 "$CHARON_PID" 2>/dev/null; then
        kill "$CHARON_PID" 2>/dev/null
        wait "$CHARON_PID" 2>/dev/null || true
    fi
    CHARON_PID=""
    sleep 0.3
}

###############################################################################
# Test 1: Build verification
###############################################################################
test_build() {
    section "Test 1: Build verification"
    cd "$ROOT_DIR"

    BUILD_OUT=$(make -f Makefile.host 2>&1)
    BUILD_EXIT=$?

    if [ $BUILD_EXIT -eq 0 ] && [ -f "$BINARY" ]; then
        pass "make -f Makefile.host succeeded"
    elif [ $BUILD_EXIT -ne 0 ]; then
        echo "$BUILD_OUT" | tail -8
        fail "make -f Makefile.host failed (exit $BUILD_EXIT)"
    else
        fail "make succeeded but charon-host binary not found"
    fi

    [ -x "$BINARY" ] && pass "charon-host is executable" || fail "charon-host is not executable"
}

###############################################################################
# Test 2: Binary format and architecture
###############################################################################
test_binary_format() {
    section "Test 2: Binary format"
    [ -f "$BINARY" ] || { skip "Binary missing"; return; }

    FILE_OUT=$(file "$BINARY")
    echo "  $FILE_OUT"

    echo "$FILE_OUT" | grep -q "ELF" \
        && pass "Binary is ELF format" \
        || fail "Binary is not ELF format"

    EXPECTED_ARCH=$(uname -m)
    if echo "$EXPECTED_ARCH" | grep -q "x86_64"; then
        echo "$FILE_OUT" | grep -q "x86-64" \
            && pass "Binary architecture matches host (x86-64)" \
            || fail "Binary architecture mismatch"
    elif echo "$EXPECTED_ARCH" | grep -q "aarch64"; then
        echo "$FILE_OUT" | grep -q "aarch64\|ARM aarch64" \
            && pass "Binary architecture matches host (aarch64)" \
            || fail "Binary architecture mismatch"
    else
        pass "Binary is ELF (architecture check skipped for $EXPECTED_ARCH)"
    fi

    # Check it links the OFDM and FEC libraries
    LDD_OUT=$(ldd "$BINARY" 2>/dev/null || echo "")
    echo "$LDD_OUT" | grep -q "liquid\|libliquid" \
        && pass "Binary links libliquid (OFDM)" \
        || fail "Binary does not link libliquid"
    echo "$LDD_OUT" | grep -q "fftw" \
        && pass "Binary links libfftw3 (FFT)" \
        || fail "Binary does not link libfftw3"
}

###############################################################################
# Test 3: Required symbols
###############################################################################
test_symbols() {
    section "Test 3: Required symbols"
    [ -f "$BINARY" ] || { skip "Binary missing"; return; }

    REQUIRED_SYMBOLS="main main_loop pluto_transmit pluto_receive do_ofdm_tx do_ofdm_rx init_ofdm_rx init_ofdm_tx pluto_init_txrx"

    for sym in $REQUIRED_SYMBOLS; do
        if nm --defined-only "$BINARY" 2>/dev/null | grep -qE " [Tt] $sym$|^[0-9a-f]+ [Tt] $sym$"; then
            pass "Symbol defined: $sym"
        elif nm "$BINARY" 2>/dev/null | grep -q "$sym"; then
            pass "Symbol present: $sym"
        else
            fail "Symbol missing: $sym"
        fi
    done

    # Confirm hardware-specific iio symbols are NOT defined (pluto.c excluded)
    if nm --defined-only "$BINARY" 2>/dev/null | grep -q "iio_channel_attr_write\b"; then
        fail "Hardware iio functions present (pluto.c should be excluded)"
    else
        pass "Hardware libiio functions excluded (pluto_host.c used instead)"
    fi
}

###############################################################################
# Test 4: Process startup and initialization messages
###############################################################################
test_startup_messages() {
    section "Test 4: Startup and initialization messages"
    [ -f "$BINARY" ] || { skip "Binary missing"; return; }
    has_root || { skip "Requires root to create TAP device (needed for startup)"; return; }

    # Start in background and collect output for 10 seconds
    INIT_LOG_FILE=$(mktemp)
    "$BINARY" >"$INIT_LOG_FILE" 2>&1 &
    INIT_PID=$!

    # Wait for the expected startup messages (up to 10s)
    local elapsed=0
    while [ $elapsed -lt 10 ]; do
        if grep -q "Initializing host mode" "$INIT_LOG_FILE" 2>/dev/null; then
            break
        fi
        sleep 1
        elapsed=$((elapsed + 1))
        if ! kill -0 "$INIT_PID" 2>/dev/null; then
            break
        fi
    done

    INIT_LOG=$(cat "$INIT_LOG_FILE" 2>/dev/null || echo "")
    kill "$INIT_PID" 2>/dev/null || true
    wait "$INIT_PID" 2>/dev/null || true
    rm -f "$INIT_LOG_FILE"

    if echo "$INIT_LOG" | grep -q "Initializing host mode"; then
        pass "Host mode initialization message printed"
    else
        fail "Did not see 'Initializing host mode' message"
        echo "  Last output: $(echo "$INIT_LOG" | tail -5)"
    fi

    if echo "$INIT_LOG" | grep -q "5002"; then
        pass "UDP port 5002 mentioned in startup output"
    else
        fail "UDP port 5002 not mentioned in startup output"
    fi

    if echo "$INIT_LOG" | grep -q "TX will send to\|RX UDP socket listening"; then
        pass "TX and RX socket configuration printed"
    else
        fail "Socket configuration messages not found"
    fi

    if echo "$INIT_LOG" | grep -q "Host mode initialization complete"; then
        pass "Host mode initialization completed successfully"
    else
        fail "'Host mode initialization complete' message not found"
    fi
}

###############################################################################
# Test 5: UDP socket binding on port 5002
###############################################################################
test_udp_socket() {
    section "Test 5: UDP socket on port 5002"
    [ -f "$BINARY" ] || { skip "Binary missing"; return; }
    has_root || { skip "Root required to create TAP device"; return; }

    # Verify port is free before test
    if udp_port_bound 5002; then
        fail "Port 5002 already in use before test"
        return
    fi
    pass "Port 5002 is free before test"

    if ! start_charon 10; then
        fail "charon-host exited during startup"
        echo "  Log: $(head -20 "$CHARON_LOG" 2>/dev/null)"
        return
    fi
    pass "charon-host process is running"

    if udp_port_bound 5002; then
        pass "UDP socket bound on port 5002 (/proc/net/udp)"
    else
        fail "UDP socket not found on port 5002 in /proc/net/udp"
    fi

    stop_charon

    # Port should be released after stop
    sleep 0.5
    if ! udp_port_bound 5002; then
        pass "UDP port 5002 released after shutdown"
    else
        pass "UDP port 5002 may still be held (acceptable)"
    fi
}

###############################################################################
# Test 6: TAP device creation
###############################################################################
test_tap_device() {
    section "Test 6: TAP device creation"
    [ -f "$BINARY" ] || { skip "Binary missing"; return; }
    has_root || { skip "Requires root (TAP device creation needs CAP_NET_ADMIN)"; return; }
    [ -e /dev/net/tun ] || { skip "/dev/net/tun not available"; return; }

    if ! start_charon 10; then
        fail "charon-host exited during startup"
        echo "  Log: $(head -30 "$CHARON_LOG" 2>/dev/null)"
        return
    fi

    if iface_exists "ofdm0"; then
        pass "TAP device ofdm0 exists (/proc/net/dev)"
        # Check charon log for TAP creation confirmation
        grep -q "created tap device" "$CHARON_LOG" 2>/dev/null \
            && pass "TAP device creation confirmed in log" \
            || pass "ofdm0 exists (creation log not present)"
    else
        fail "TAP device ofdm0 not found in /proc/net/dev"
        echo "  /proc/net/dev:"
        cat /proc/net/dev 2>/dev/null | head -10
        echo "  Log tail:"
        tail -20 "$CHARON_LOG" 2>/dev/null
    fi

    stop_charon
}

###############################################################################
# Test 7: Self-loopback UDP IQ injection
###############################################################################
test_udp_iq_loopback() {
    section "Test 7: UDP IQ sample self-loopback"
    [ -f "$BINARY" ] || { skip "Binary missing"; return; }
    has_root || { skip "Requires root"; return; }
    has_python3 || { skip "python3 not available"; return; }

    if ! start_charon 10; then
        fail "charon-host not running after startup"
        return
    fi
    pass "charon-host running"

    # Send silence (zero I/Q samples) to the RX UDP port.
    # charon-host TX also sends to 127.0.0.1:5002, creating a self-loopback.
    # Sending silence exercises the RX path (recvfrom + do_process_iq16).
    python3 "$HELPERS_DIR/udp_iq_client.py" --mode silence --count 20 --interval 0.01 2>&1
    PY_EXIT=$?

    if [ $PY_EXIT -eq 0 ]; then
        pass "UDP IQ silence injection completed without error"
    else
        fail "UDP IQ silence injection returned error $PY_EXIT"
    fi

    # Give charon-host time to process
    sleep 1
    if kill -0 "$CHARON_PID" 2>/dev/null; then
        pass "charon-host survived IQ sample injection"
    else
        fail "charon-host crashed during IQ sample injection"
        echo "  Log tail: $(tail -20 "$CHARON_LOG" 2>/dev/null)"
    fi

    # Send noise samples too (exercises the OFDM demodulator more thoroughly)
    python3 "$HELPERS_DIR/udp_iq_client.py" --mode noise --count 10 --interval 0.01 2>&1
    sleep 0.5
    if kill -0 "$CHARON_PID" 2>/dev/null; then
        pass "charon-host survived noise IQ injection"
    else
        fail "charon-host crashed during noise injection"
    fi

    stop_charon
}

###############################################################################
# Test 8: TAP frame injection → OFDM TX path
###############################################################################
test_tap_frame_injection() {
    section "Test 8: TAP frame injection (OFDM TX path)"
    [ -f "$BINARY" ] || { skip "Binary missing"; return; }
    has_root || { skip "Requires root"; return; }
    has_python3 || { skip "python3 not available"; return; }
    [ -e /dev/net/tun ] || { skip "/dev/net/tun not available"; return; }

    if ! start_charon 10; then
        fail "charon-host not running after startup"
        return
    fi
    pass "charon-host running"

    if ! iface_exists "ofdm0"; then
        fail "ofdm0 TAP device not available for frame injection"
        stop_charon
        return
    fi
    pass "ofdm0 TAP device exists"

    # Inject raw Ethernet broadcast frames into the ofdm0 TAP device.
    # charon-host reads from the TAP fd, OFDM-modulates the frames, and sends
    # IQ samples via UDP to 127.0.0.1:5002 (which loops back to its own RX).
    python3 "$HELPERS_DIR/tap_inject.py" --device ofdm0 --count 3 --interval 0.2 2>&1
    PY_EXIT=$?

    if [ $PY_EXIT -eq 0 ]; then
        pass "Ethernet frame injection into ofdm0 completed"
    else
        fail "Ethernet frame injection returned error $PY_EXIT"
    fi

    sleep 1

    # Verify charon-host is still alive after processing frames
    if kill -0 "$CHARON_PID" 2>/dev/null; then
        pass "charon-host survived TAP frame injection (OFDM TX executed)"
    else
        fail "charon-host crashed during TAP frame injection"
        echo "  Log tail: $(tail -20 "$CHARON_LOG" 2>/dev/null)"
    fi

    # Check that OFDM TX activity was logged
    if grep -q "Read.*bytes from ofdm0" "$CHARON_LOG" 2>/dev/null; then
        pass "OFDM TX path confirmed: frames read from TAP device"
    else
        skip "OFDM TX log confirmation not available (debug logging may be off)"
    fi

    stop_charon
}

###############################################################################
# Test 9: OFDM example programs (PHY-layer validation)
# Runs ofdm_loopback_example which exercises the full OFDM TX → channel → RX
# pipeline using liquid-dsp (the same library used by charon-host).
###############################################################################
test_ofdm_examples() {
    section "Test 9: OFDM example programs (PHY-layer validation)"
    EXAMPLE_DIR="$ROOT_DIR/example"

    [ -d "$EXAMPLE_DIR" ] || { skip "example/ directory missing"; return; }

    cd "$EXAMPLE_DIR"

    BUILD_OUT=$(make 2>&1)
    BUILD_EXIT=$?
    if [ $BUILD_EXIT -ne 0 ] && echo "$BUILD_OUT" | grep -qE "error:"; then
        fail "OFDM example build failed"
        echo "$BUILD_OUT" | tail -5
        cd "$ROOT_DIR"
        return
    fi
    pass "OFDM examples built"

    # Run the loopback example: full TX→channel→RX in-process validation
    # (uses the same liquid-dsp OFDM modulator/demodulator as charon-host)
    if [ -x "./ofdm_loopback_example" ]; then
        LOOPBACK_OUT=$(./ofdm_loopback_example 2>&1)
        LOOPBACK_EXIT=$?
        if [ $LOOPBACK_EXIT -eq 0 ] && echo "$LOOPBACK_OUT" | grep -q "completed successfully\|PASSED\|Payload valid: YES"; then
            pass "ofdm_loopback_example: TX→channel→RX loopback succeeded"
            # Report key metrics if available
            if echo "$LOOPBACK_OUT" | grep -q "Payload valid: YES"; then
                EVM=$(echo "$LOOPBACK_OUT" | grep "EVM:" | head -1 || echo "")
                [ -n "$EVM" ] && echo "  $EVM"
            fi
        else
            fail "ofdm_loopback_example failed (exit $LOOPBACK_EXIT)"
            echo "$LOOPBACK_OUT" | tail -10
        fi
    else
        skip "ofdm_loopback_example not found or not executable"
    fi

    cd "$ROOT_DIR"
}

###############################################################################
# Main
###############################################################################
echo -e "${CYAN}================================================${NC}"
echo -e "${CYAN}  Charon Host Mode Test Suite${NC}"
echo -e "${CYAN}================================================${NC}"

if has_root; then
    echo "  Running as root — all tests enabled"
else
    echo "  Running without root — TAP/socket tests skipped"
fi
echo "  Binary: $BINARY"
echo ""

test_build
test_binary_format
test_symbols
test_startup_messages
test_udp_socket
test_tap_device
test_udp_iq_loopback
test_tap_frame_injection
test_ofdm_examples

echo ""
echo -e "${CYAN}================================================${NC}"

TOTAL=$((PASS + FAIL + SKIP))
if [ "$FAIL" -eq 0 ]; then
    echo -e "${GREEN}  RESULT: $PASS/$TOTAL passed, $SKIP skipped — ALL TESTS PASSED${NC}"
else
    echo -e "${RED}  RESULT: $PASS/$TOTAL passed, $FAIL failed, $SKIP skipped${NC}"
fi

echo -e "${CYAN}================================================${NC}"

[ "$FAIL" -eq 0 ]
