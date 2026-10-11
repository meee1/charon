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
# Needs root (namespaces, TAP), iproute2, iperf3, and sim/build/{rfchan,charon_sim}.
#
#   sudo sim/netns_run.sh                         # 3-node line A-B-C, auto batman
#   sudo sim/netns_run.sh --topology mesh --nodes 2
#   sudo sim/netns_run.sh --batman off -s node.2.ppm=3
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
BUILD=$HERE/build
TOPO=line
N=3
BATMAN=auto
DURATION=10
LOSS=70
CHAN=()
KEEP=0

while [ $# -gt 0 ]; do
  case "$1" in
    --topology) TOPO=$2; shift 2 ;;
    --nodes) N=$2; shift 2 ;;
    --batman) BATMAN=$2; shift 2 ;;
    --duration) DURATION=$2; shift 2 ;;
    --loss) LOSS=$2; shift 2 ;;
    -s) CHAN+=("-s" "$2"); shift 2 ;;
    --keep) KEEP=1; shift ;;
    -h|--help) sed -n '23,46p' "$0"; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done

[ "$(id -u)" = 0 ] || { echo "needs root" >&2; exit 2; }
for b in rfchan charon_sim; do
  [ -x "$BUILD/$b" ] || { echo "missing $BUILD/$b: run 'make -C sim'" >&2; exit 2; }
done
[ "$TOPO" = line ] && [ "$N" -lt 3 ] && { echo "line topology needs >= 3 nodes" >&2; exit 2; }

if [ "$BATMAN" = auto ]; then
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

PIDS=()
cleanup() {
  for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
  sleep 0.5
  for p in "${PIDS[@]}"; do kill -9 "$p" 2>/dev/null; done
  for i in $(seq 0 $((N - 1))); do ip netns del "$(ns_of $i)" 2>/dev/null; done
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
echo "topology=$TOPO nodes=$N batman=$BATMAN run=$RUN"
for i in $(seq 0 $((N - 1))); do
  ns=$(ns_of $i)
  ip netns add "$ns"
  ip -n "$ns" link set lo up
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
  ip netns exec "$ns" env CHARON_ENV_FILE="$RUN/node$i.env" CHARON_SIM_NODE=$i \
    CHARON_SIM_TX_EP=$TX_EP CHARON_SIM_RX_EP=$RX_EP \
    "$BUILD/charon_sim" > "$RUN/node$i.log" 2>&1 &
  PIDS+=($!)
done

# wait for every ofdm0 to come up
for i in $(seq 0 $((N - 1))); do
  for t in $(seq 1 100); do
    ip -n "$(ns_of $i)" link show ofdm0 2>/dev/null | grep -q "UP" && break
    sleep 0.2
  done
done
sleep 2

# --- addressing / routing -------------------------------------------------------
if [ "$BATMAN" = off ]; then
  for i in $(seq 0 $((N - 1))); do
    ns=$(ns_of $i)
    ip -n "$ns" addr add "$(ip_of $i)/32" dev ofdm0
    ip netns exec "$ns" sysctl -qw net.ipv4.ip_forward=1 net.ipv4.conf.all.send_redirects=0 \
      net.ipv4.conf.ofdm0.send_redirects=0 net.ipv4.conf.all.accept_redirects=0 \
      net.ipv4.conf.all.rp_filter=0 net.ipv4.conf.ofdm0.rp_filter=0
    for j in $(seq 0 $((N - 1))); do
      [ "$i" = "$j" ] && continue
      if [ "$TOPO" = line ] && [ $(( j > i ? j - i : i - j )) -gt 1 ]; then
        nh=$(( j > i ? i + 1 : i - 1 ))
        ip -n "$ns" route add "$(ip_of $j)/32" via "$(ip_of $nh)" dev ofdm0 onlink
      else
        ip -n "$ns" route add "$(ip_of $j)/32" dev ofdm0
      fi
    done
  done
else
  # batman: give it time to learn originators
  sleep 15
fi

# --- tests --------------------------------------------------------------------
FAIL=0
check() {  # name, command...
  local name=$1; shift
  if "$@" > "$RUN/$(echo "$name" | tr ' /' '__').out" 2>&1; then
    echo "  ok    $name"
  else
    echo "  FAIL  $name   (see $RUN)"; FAIL=1
  fi
}
first=$(ns_of 0); last=$(ns_of $((N - 1)))
check "ping node0 -> node1" ip netns exec "$first" ping -c 5 -i 0.5 -W 3 "$(ip_of 1)"
check "ping node0 -> node$((N - 1))" ip netns exec "$first" ping -c 10 -i 0.5 -W 5 "$(ip_of $((N - 1)))"

ip netns exec "$last" iperf3 -s -1 > "$RUN/iperf3_server.log" 2>&1 &
PIDS+=($!)
sleep 1
check "iperf3 TCP node0 -> node$((N - 1)) (${DURATION}s)" \
  sh -c "ip netns exec $first iperf3 -c $(ip_of $((N - 1))) -t $DURATION -J > $RUN/iperf3.json"
rate=$(python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print('%.1f' % (d['end']['sum_received']['bits_per_second']/1e3))" \
  "$RUN/iperf3.json" 2>/dev/null)
[ -n "$rate" ] && echo "  iperf3 throughput: $rate kbit/s"
kill "${PIDS[0]}" 2>/dev/null; sleep 0.5
grep -h "overruns" "$RUN/rfchan.log" 2>/dev/null

if [ "$KEEP" = 1 ]; then
  echo "namespaces kept; press enter to tear down"; read -r _
fi
[ "$FAIL" = 0 ] && echo "PASS" || echo "FAIL"
exit $FAIL
