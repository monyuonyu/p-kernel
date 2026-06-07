#!/bin/bash
# ---------------------------------------------------------------------------
# 4-node, 2-region hierarchical distributed KV attention over the ./relay.
#
# Demonstrates the regions architecture (docs/architecture/regions.md): nodes
# are split into two latency clusters by a simulated RTT penalty, and DKVA
# aggregates attention hierarchically — dense inside each region, sparse
# across regions — to reconstruct the exact global attention at O(region^2)
# + O(#regions) traffic instead of O(N^2).
#
#   zone 0 (region A): node0 (id1, requester+coord), node1 (id2)
#   zone 1 (region B): node2 (id3, coordinator),     node3 (id4)
#
# Topic scopes: Q=global (every node computes a partial), resp/<n>=region
# (per-node partials stay in-region), rsum/<rid>=global (only each region's
# coordinator summary crosses region boundaries).
#
# So node0's query reaches all four nodes; node3's partial stays inside
# region B and is folded by node2's coordinator into one region summary,
# which is the only region-B traffic node0 sees. node0 aggregates its own
# region directly (node1) plus region B's summary = all four nodes' KV.
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
echo "===== node 1 — DKVA (hierarchical: own region + remote summary) ====="
grep -E "Q broadcast|resp from node|region summary rid|aggregated|=> class" /tmp/pk4_node1.log
echo
echo "===== region B — internal partials folded into one cross-region summary ====="
echo "node 3 (id3, coordinator) published region summary: $(grep -c 'region summary published' /tmp/pk4_node3.log)"
echo "node 4 (id4) computed its partial (stays in region B): $(grep -c 'responded to node 0' /tmp/pk4_node4.log)"
echo
echo "[demo] done. full logs in /tmp/pk4_node{1..4}.log /tmp/pk4_relay.log"
