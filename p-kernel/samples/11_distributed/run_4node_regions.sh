#!/bin/bash
# ---------------------------------------------------------------------------
# 4-node, 2-region distributed KV attention over the public ./relay.
#
# Demonstrates the regions architecture (docs/architecture/regions.md): nodes
# are split into two latency clusters by a simulated RTT penalty, and DKVA's
# region-scoped topics confine each requester's distributed attention to its
# own region.
#
#   zone 0 (region A): node0 (id1), node1 (id2)
#   zone 1 (region B): node2 (id3), node3 (id4)
#
# With PKERNEL_RTT_ZONE_SIZE=2 the cross-zone RTT is inflated past tau=50ms,
# so node0's region is {0,1}. Its DKVA query reaches only node1; the region-B
# nodes never see it. node0 aggregates exactly its one same-region peer.
#
# Usage:   ./run_4node_regions.sh
# Watch:   /tmp/pk4_node{1..4}.log  /tmp/pk4_relay.log
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "unsupported host arch $(uname -m)"; exit 1 ;;
esac

[ -x "$BOOT/p-kernel" ]    || make -C "$BOOT"        >/dev/null || exit 1
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay"  >/dev/null || exit 1

export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
export PKERNEL_RELAY_HOST=127.0.0.1
export PKERNEL_RELAY_PORT=7416
# Two latency zones of 2 nodes each; cross-zone adds 200ms (> tau=50ms).
export PKERNEL_RTT_ZONE_SIZE=2
export PKERNEL_RTT_ZONE_PENALTY=200

PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

echo "[demo] starting relay on :$PKERNEL_RELAY_PORT"
"$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" -v >/tmp/pk4_relay.log 2>&1 & PIDS+=($!)
sleep 1

echo "[demo] starting nodes 2,3,4 (region A peer + region B pair)"
PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 "$BOOT/p-kernel" </dev/null >/tmp/pk4_node2.log 2>&1 & PIDS+=($!)
PKERNEL_NODE_ID=3 PKERNEL_AUTONET=1 "$BOOT/p-kernel" </dev/null >/tmp/pk4_node3.log 2>&1 & PIDS+=($!)
PKERNEL_NODE_ID=4 PKERNEL_AUTONET=1 "$BOOT/p-kernel" </dev/null >/tmp/pk4_node4.log 2>&1 & PIDS+=($!)
sleep 2

echo "[demo] starting node 1 (region-A requester) and driving inference"
{
  sleep 13                 # let SWIM measure RTT and regions settle
  echo "region"
  sleep 1
  echo "infer 50 20 90 5"
  sleep 3
  echo "exit"
} | PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 "$BOOT/p-kernel" >/tmp/pk4_node1.log 2>&1

echo
echo "===== node 1 (region A) — region view ====="
grep -E "\[region\]" /tmp/pk4_node1.log
echo
echo "===== node 1 — DKVA (region-confined) ====="
grep -E "Q broadcast|resp from node|aggregated|=> class" /tmp/pk4_node1.log
echo
echo "===== region B saw node 1's query? (expect 0 each) ====="
echo "node 3 (id3) responded to node 0: $(grep -c 'responded to node 0' /tmp/pk4_node3.log)"
echo "node 4 (id4) responded to node 0: $(grep -c 'responded to node 0' /tmp/pk4_node4.log)"
echo "node 2 (id2, region A) responded to node 0: $(grep -c 'responded to node 0' /tmp/pk4_node2.log)"
echo
echo "[demo] done. full logs in /tmp/pk4_node{1..4}.log /tmp/pk4_relay.log"
