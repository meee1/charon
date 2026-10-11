#!/bin/bash
# MIT License
#
# Copyright (c) 2026 Charon contributors
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#
# Full-stack Charon simulation: one network namespace per node, each running
# charon_sim with a real ofdm0 TAP device, connected through rfchan.  Then
# ping and iperf3 run across the simulated RF links.
#
# Needs iproute2, iperf3, util-linux (unshare/nsenter) and
# sim/build/{rfchan,charon_sim}.
#
#   sudo sim/netns_run.sh                         # 3-node line A-B-C, auto batman
#   sudo sim/netns_run.sh --topology mesh --nodes 2
#   sudo sim/netns_run.sh --batman off -s node.2.ppm=3
#   sim/netns_run.sh                              # as a normal user: rootless
#
# Rootless mode (default when not root, or --rootless): each node runs in its
# own user + network namespace (unshare -Urn), where it is root and can create
# ofdm0; the script configures it with nsenter.  Needs unprivileged user
# namespaces (some distros restrict them, e.g. Ubuntu's
# kernel.apparmor_restrict_unprivileged_userns) and a /dev/net/tun the user
# can open (0666 on most distros).  batman-adv is not available rootless.
#
# Routing:
#   batman on   charon's own batman-adv setup (needs the batman_adv kernel
#               module and batctl); each namespace gets a dummy usb0 so the
#               mesh-bridge layout matches a Pluto
#   batman off  no_batman=1: ofdm0 is a plain L2 link; this script assigns
#               /32 addresses and static routes (line: A and C route via B,
#               which forwards)
#   auto        batman if the kernel supports it, else off
#
# Exit status 0 if all ping/iperf checks pass.

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=${CHARON_SIM_BUILD:-$HERE/build}   # override e.g. for an ASan build
TOPO=line
N=3
BATMAN=auto
DURATION=10
LOSS=70
CHAN=()
KEEP=0
ROOTLESS=auto

while [ $# -gt 0 ]; do
  case "$1" in
    --topology) TOPO=$2; shift 2 ;;
    --nodes) N=$2; shift 2 ;;
    --batman) BATMAN=$2; shift 2 ;;
    --duration) DURATION=$2; shift 2 ;;
    --loss) LOSS=$2; shift 2 ;;
    -s) CHAN+=("-s" "$2"); shift 2 ;;
    --keep) KEEP=1; shift ;;
    --rootless) ROOTLESS=1; shift ;;
    -h|--help) sed -n '24,54p' "$0"; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done

if [ "$ROOTLESS" = auto ]; then
  [ "$(id -u)" = 0 ] && ROOTLESS=0 || ROOTLESS=1
fi
if [ "$ROOTLESS" = 1 ]; then
  [ -r /dev/net/tun ] && [ -w /dev/net/tun ] || {
    echo "rootless: /dev/net/tun is not readable/writable by $(id -un) ($(ls -l /dev/net/tun | cut -d' ' -f1))" >&2; exit 2; }
  unshare -Urn true 2>/dev/null || {
    echo "rootless: unprivileged user namespaces are not available (unshare -Urn failed)" >&2; exit 2; }
  if [ "$BATMAN" = on ]; then echo "batman-adv is not supported rootless" >&2; exit 2; fi
  BATMAN=off
fi
for b in rfchan charon_sim; do
  [ -x "$BUILD/$b" ] || { echo "missing $BUILD/$b: run 'make -C sim'" >&2; exit 2; }
done
[ "$DURATION" -lt 10 ] && { echo "--duration must be >= 10 s (TCP needs time to ramp over the slow link)" >&2; exit 2; }
[ "$TOPO" = line ] && [ "$N" -lt 3 ] && { echo "line topology needs >= 3 nodes" >&2; exit 2; }

if [ "$BATMAN" = auto ] && [ "$ROOTLESS" = 0 ]; then
  if ip link add dev batprobe0 type batadv 2>/dev/null; then
    ip link del batprobe0; BATMAN=on
  else
    BATMAN=off
  fi
fi
if [ "$BATMAN" = on ] && ! command -v batctl >/dev/null; then
  echo "batman requested but batctl not installed" >&2; exit 2
fi

RUN=$HERE/runs/netns_${TOPO}${N}
rm -rf "$RUN"; mkdir -p "$RUN"
TAG=netns$$
TX_EP=ipc:///tmp/charon_sim_${TAG}_tx
RX_EP=ipc:///tmp/charon_sim_${TAG}_rx
ip_of() { echo "10.0.0.$(( $1 + 1 ))"; }
ns_of() { echo "cs${TAG}_$1"; }

# Run a command inside node i's network namespace (as root there).
NODE_PID=()
nx() {
  local i=$1; shift
  if [ "$ROOTLESS" = 1 ]; then
    nsenter -t "${NODE_PID[$i]}" -U -n --preserve-credentials -- "$@"
  else
    ip netns exec "$(ns_of $i)" "$@"
  fi
}

PIDS=()
cleanup() {
  for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
  sleep 0.5
  for p in "${PIDS[@]}"; do kill -9 "$p" 2>/dev/null; done
  if [ "$ROOTLESS" = 0 ]; then
    for i in $(seq 0 $((N - 1))); do ip netns del "$(ns_of $i)" 2>/dev/null; done
  fi
  rm -f "/tmp/charon_sim_${TAG}_tx" "/tmp/charon_sim_${TAG}_rx"
}
trap cleanup EXIT

# --- channel ----------------------------------------------------------------
LINKS=()
if [ "$TOPO" = line ]; then
  LINKS+=("-s" "link.*.loss_db=inf")
  for i in $(seq 0 $((N - 2))); do LINKS+=("-s" "link.$i-$((i + 1)).loss_db=$LOSS"); done
else
  LINKS+=("-s" "link.*.loss_db=$LOSS")
fi
"$BUILD/rfchan" -s nodes=$N -s tx_ep=$TX_EP -s rx_ep=$RX_EP "${LINKS[@]}" "${CHAN[@]}" \
  -g "$RUN/gt.jsonl" > "$RUN/rfchan.log" 2>&1 &
PIDS+=($!)
sleep 0.3

# --- nodes ------------------------------------------------------------------
echo "topology=$TOPO nodes=$N batman=$BATMAN rootless=$ROOTLESS run=$RUN"
for i in $(seq 0 $((N - 1))); do
  ns=$(ns_of $i)
  if [ "$ROOTLESS" = 0 ]; then
    ip netns add "$ns"
    ip -n "$ns" link set lo up
  fi
  {
    echo enable_charon=1
    echo freq_rxtx_hz=915000000
    echo ref_correction_ppm=0
    echo cpu_mask=0
    echo ipaddr=$(ip_of $i)
    [ "$BATMAN" = off ] && echo no_batman=1
    [ "$BATMAN" = on ] && echo bat_ogm_interval=1000
  } > "$RUN/node$i.env"
  if [ "$BATMAN" = on ]; then
    ip -n "$ns" link add usb0 type dummy
  fi
  if [ "$ROOTLESS" = 1 ]; then
    # unshare execs (no fork), so $! is charon_sim itself: nsenter target
    unshare -Urn env CHARON_ENV_FILE="$RUN/node$i.env" CHARON_SIM_NODE=$i \
      CHARON_SIM_TX_EP=$TX_EP CHARON_SIM_RX_EP=$RX_EP \
      "$BUILD/charon_sim" > "$RUN/node$i.log" 2>&1 &
    NODE_PID[$i]=$!
  else
    ip netns exec "$ns" env CHARON_ENV_FILE="$RUN/node$i.env" CHARON_SIM_NODE=$i \
      CHARON_SIM_TX_EP=$TX_EP CHARON_SIM_RX_EP=$RX_EP \
      "$BUILD/charon_sim" > "$RUN/node$i.log" 2>&1 &
  fi
  PIDS+=($!)
done
if [ "$ROOTLESS" = 1 ]; then
  sleep 0.5
  for i in $(seq 0 $((N - 1))); do nx $i ip link set lo up; done
fi

# wait for every ofdm0 to come up
for i in $(seq 0 $((N - 1))); do
  for t in $(seq 1 100); do
    nx $i ip link show ofdm0 2>/dev/null | grep -q "UP" && break
    sleep 0.2
  done
done
sleep 2

# --- addressing / routing -------------------------------------------------------
if [ "$BATMAN" = off ]; then
  for i in $(seq 0 $((N - 1))); do
    nx $i ip addr add "$(ip_of $i)/32" dev ofdm0
    nx $i sysctl -qw net.ipv4.ip_forward=1 net.ipv4.conf.all.send_redirects=0 \
      net.ipv4.conf.ofdm0.send_redirects=0 net.ipv4.conf.all.accept_redirects=0 \
      net.ipv4.conf.all.rp_filter=0 net.ipv4.conf.ofdm0.rp_filter=0
    for j in $(seq 0 $((N - 1))); do
      [ "$i" = "$j" ] && continue
      if [ "$TOPO" = line ] && [ $(( j > i ? j - i : i - j )) -gt 1 ]; then
        nh=$(( j > i ? i + 1 : i - 1 ))
        nx $i ip route add "$(ip_of $j)/32" via "$(ip_of $nh)" dev ofdm0 onlink
      else
        nx $i ip route add "$(ip_of $j)/32" dev ofdm0
      fi
    done
  done
else
  # batman: give it time to learn originators
  sleep 15
fi

# Warm up neighbour (ARP) entries between adjacent nodes so the first test
# packets don't race ARP over the slow link (batman's OGMs do this on a mesh)
if [ "$BATMAN" = off ]; then
  for i in $(seq 0 $((N - 2))); do
    nx $i ping -c 3 -i 0.5 -W 3 "$(ip_of $((i + 1)))" > /dev/null 2>&1
    nx $((i + 1)) ping -c 1 -W 3 "$(ip_of $i)" > /dev/null 2>&1
  done
fi

# --- tests --------------------------------------------------------------------
FAIL=0
check() {  # name, command...
  local name=$1; shift
  # file names: only [A-Za-z0-9_.-] (CI artifact upload rejects > ( etc.)
  if "$@" > "$RUN/$(echo "$name" | tr -c 'A-Za-z0-9_.-' '_').out" 2>&1; then
    echo "  ok    $name"
  else
    echo "  FAIL  $name   (see $RUN)"; FAIL=1
  fi
}
last=$((N - 1))
check "ping node0 -> node1" nx 0 ping -c 5 -i 0.5 -W 3 "$(ip_of 1)"
[ "$last" -gt 1 ] && check "ping node0 -> node$last" nx 0 ping -c 10 -i 0.5 -W 5 "$(ip_of $last)"

nx $last iperf3 -s -1 > "$RUN/iperf3_server.log" 2>&1 &
PIDS+=($!)
sleep 1
# Pass only if data actually arrived (> 10 kbit/s at the receiver)
iperf_client() {
  nx 0 timeout $((DURATION + 40)) iperf3 -c "$(ip_of $last)" -t "$DURATION" -J > "$RUN/iperf3.json" || return 1
  python3 -c "import json,sys; sys.exit(0 if json.load(open(sys.argv[1]))['end']['sum_received']['bits_per_second'] > 10e3 else 1)" \
    "$RUN/iperf3.json"
}
check "iperf3 TCP node0 -> node$last (${DURATION}s)" iperf_client
rate=$(python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print('%.1f' % (d['end']['sum_received']['bits_per_second']/1e3))" \
  "$RUN/iperf3.json" 2>/dev/null)
[ -n "$rate" ] && echo "  iperf3 throughput: $rate kbit/s (received)"

# On failure, dump per-node network state and log tails (for CI logs)
if [ "$FAIL" != 0 ]; then
  for i in $(seq 0 $((N - 1))); do
    echo "--- node$i: addr / route / neigh"
    nx $i ip -br addr 2>&1 | sed 's/^/    /'
    nx $i ip route 2>&1 | sed 's/^/    /'
    nx $i ip neigh 2>&1 | sed 's/^/    /'
    echo "--- node$i: log tail"
    tr '\r' '\n' < "$RUN/node$i.log" | grep -av 'samples/sec\|noise_floor\|^$' | tail -15 | sed 's/^/    /'
  done
  echo "--- iperf3 client error / server log"
  python3 -c "import json,sys; print('    ', json.load(open(sys.argv[1])).get('error'))" "$RUN/iperf3.json" 2>&1
  tail -5 "$RUN/iperf3_server.log" 2>/dev/null | sed 's/^/    /'
  for i in 0 $last; do
    echo "--- node$i: TCP sockets and IP/TCP error counters"
    nx $i ss -tni 2>&1 | sed 's/^/    /'
    nx $i nstat -az 2>/dev/null | grep -E "CsumErrors|InErrors|TcpRetransSegs|TcpOutRsts|TcpInSegs|TcpOutSegs|IpInHdrErrors|IpInAddrErrors|IpForwDatagrams|TcpExtTCPSynRetrans|TcpAttemptFails|TcpExtListenDrops|TcpExtTCPTimeouts" | sed 's/^/    /'
  done
  echo "--- rfchan"; sed 's/^/    /' "$RUN/rfchan.log"
fi

if [ "$KEEP" = 1 ]; then
  echo "namespaces kept; press enter to tear down"; read -r _
fi
kill "${PIDS[0]}" 2>/dev/null; sleep 0.5     # rfchan last, so --keep leaves a live channel
grep -h "overruns" "$RUN/rfchan.log" 2>/dev/null
[ "$FAIL" = 0 ] && echo "PASS" || echo "FAIL"
exit $FAIL
