#!/bin/bash
# ---------------------------------------------------------------------------
# Phase D end-to-end: three libpkernel.so nodes (the SAME artifact the APK
# ships) join a Phase B v2 relay mesh and form a region from *measured*
# SWIM RTT — no PKERNEL_RTT_ZONE_* simulator. This is the "real water
# through the pipe" check: a phone running the APK behaves exactly like
# one of these so_node processes (nativeBoot dlopens this .so, AUTONET on,
# relay configured), and never sets the sim-zone env, so its region is
# whatever its real latency neighbours turn out to be.
#
# On localhost every RTT is sub-millisecond (<< tau=50ms), so all three
# nodes coalesce into ONE region — the honest outcome for co-located
# nodes, and the proof that regions track *real* latency rather than the
# hand-wired zones used by run_4node_regions.sh.
#
# Usage:   make -C ../../boot/linux so_node && ./run_region_relay_so.sh
# Watch:   /tmp/so_node{1..3}.log  /tmp/so_relay.log
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BOOT="$ROOT/boot/linux"        # so_node + libpkernel.so live here (aarch64 host)

[ -x "$BOOT/so_node" ]     || make -C "$BOOT" so_node >/dev/null || exit 1
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay"   >/dev/null || exit 1

export PKERNEL_RELAY_KEY=a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5
export PKERNEL_RELAY_HOST=127.0.0.1
export PKERNEL_RELAY_PORT=27461
# NOTE: deliberately NO PKERNEL_RTT_ZONE_* — regions form from measured RTT.

PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

echo "[demo] relay on :$PKERNEL_RELAY_PORT"
"$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" -v >/tmp/so_relay.log 2>&1 & PIDS+=($!)
sleep 1

echo "[demo] starting so_node 2,3 (libpkernel.so, AUTONET, relay)"
cd "$BOOT"
PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 LD_LIBRARY_PATH=. ./so_node </dev/null >/tmp/so_node2.log 2>&1 & PIDS+=($!)
PKERNEL_NODE_ID=3 PKERNEL_AUTONET=1 LD_LIBRARY_PATH=. ./so_node </dev/null >/tmp/so_node3.log 2>&1 & PIDS+=($!)
sleep 2

echo "[demo] starting so_node 1 and reading its region/capacity view"
# End the driver with `exit` so the kernel's shell leaves cleanly instead
# of hitting EOF on stdin (which parks it in sio_read_line's keep-alive
# for(;;) idle loop and would hang this script).
(
  sleep 13          # let SWIM measure real RTT and the region settle
  echo "nodes"
  sleep 1
  echo "region"
  sleep 1
  echo "infer 50 20 90 5"
  sleep 3
  echo "dist"
  sleep 2
  echo "exit"
) | PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 LD_LIBRARY_PATH=. ./so_node >/tmp/so_node1.log 2>&1

echo
echo "===== relay: who registered ====="
grep -E "registered" /tmp/so_relay.log | head
echo
echo "===== node 1: SWIM peers + measured RTT ====="
grep -E "discovered|rtt=" /tmp/so_node1.log | head
echo
echo "===== node 1: region (from measured RTT, no sim zone) ====="
grep -E "\[region\]" /tmp/so_node1.log
echo
echo "===== node 1: capacity(N) ====="
grep -E "\[capacity\]|KV entries\)" /tmp/so_node1.log
echo
echo "[demo] done. logs: /tmp/so_node{1..3}.log /tmp/so_relay.log"
