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
# Two latency zones of 2 nodes each (size=2); the penalty is set PER RUN below
# (200ms = zoned/hierarchical, 0ms = flat-control falsifier).
export PKERNEL_RTT_ZONE_SIZE=2

# ---------------------------------------------------------------------------
# [fed-2cluster][live] — a MEASURED, FALSIFIABLE gate (federation R0, plan §3.1
# Arm B). One run drives node1 through:
#     kdds            -> prints [locality] tx_msgs/far_msgs/near_msgs (BEFORE)
#     infer ...       -> one hierarchical inference
#     kdds            -> prints [locality] again                      (AFTER)
# We diff the [locality] counters across the one inference (kdds.c:543).
#
# What the counters mean (kdds.c:262-280): a message is `far` if its topic is
# GLOBAL-scoped and the receiver is in a DIFFERENT region; `near` otherwise.
# So:
#   ZONED (2 regions): region-B's dense per-node partials are REGION-scoped and
#     never reach node1. node1 only crosses the boundary via the GLOBAL Q to
#     coordB + the ONE rsum summary back  -> far_delta is small & BOUNDED by
#     O(#region), and node1's near fan-in stays small (its own region only).
#   FLAT (1 region, penalty=0): no boundary -> far_delta == 0, but node1 must
#     collect every node's partial DIRECTLY (no summary aggregation), so its
#     near fan-in scales with the cluster size N.
#
# The gate:
#   (a) O(#region) not O(N): zoned far_delta <= (#remote_regions × K). far is
#       summary-only cross-boundary traffic; it must NOT scale with region-B's
#       node count. #remote_regions=1.
#   (b) DEGENERATION FALSIFIER: the flat control collapses the hierarchy. If
#       region-scoping were a no-op, node1 would see the SAME fan-in either way.
#       The mechanical distinguishers — both must hold:
#         (b1) zoned far_delta > 0  (a real boundary is crossed by summaries),
#              flat far_delta == 0  (no boundary exists);
#         (b2) zoned near_delta < flat near_delta — the hierarchy HIDES region-B's
#              dense chatter behind one summary; the flat run re-exposes it as
#              direct fan-in. If scoping degenerated to all-to-all the two near
#              deltas would be equal and this fails.
#   (c) ONE-MATH: node1's `=> class` answer is IDENTICAL zoned vs flat.
# ---------------------------------------------------------------------------

# run_once <penalty> <port> <tag> : start a fresh relay+4 nodes, drive node1,
#   write logs to /tmp/pk4_<tag>_node{1..4}.log, leave node1 log for parsing.
run_once() {
    local penalty="$1" port="$2" tag="$3"
    local pids=()
    PKERNEL_RELAY_PORT="$port" "$ROOT/relay/relay" -p "$port" -v \
        >/tmp/pk4_${tag}_relay.log 2>&1 & pids+=($!)
    sleep 1
    local i
    for i in 2 3 4; do
        env PKERNEL_NODE_ID=$i PKERNEL_AUTONET=1 \
            PKERNEL_RELAY_PORT="$port" PKERNEL_RTT_ZONE_PENALTY="$penalty" \
            "$BOOT/p-kernel" </dev/null >/tmp/pk4_${tag}_node$i.log 2>&1 & pids+=($!)
    done
    sleep 2
    {
      sleep 13                 # let SWIM measure RTT and regions settle
      echo "region"
      sleep 1
      echo "kdds"             # [locality] BEFORE infer
      sleep 1
      echo "infer 50 20 90 5"
      sleep 3
      echo "kdds"             # [locality] AFTER infer
      sleep 1
      echo "exit"
    } | env PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 \
            PKERNEL_RELAY_PORT="$port" PKERNEL_RTT_ZONE_PENALTY="$penalty" \
            "$BOOT/p-kernel" >/tmp/pk4_${tag}_node1.log 2>&1
    kill "${pids[@]}" 2>/dev/null; wait 2>/dev/null
}

# Pull the N-th (1-based) <name>= value from a node1 log's [locality] lines.
loc_at() { sed -n "s/.*\[locality\].* $2=\([0-9]*\).*/\1/p" "$1" | sed -n "${3}p"; }
# Pull node1's deterministic answer (=> class N) zoned vs flat.
fp_of()  { grep -oE "=> class [0-9]+" "$1" | tail -1; }

echo "[demo] ===== ZONED run (penalty=200, 2 regions) ====="
run_once 200 7416 zoned
echo "[demo] ===== FLAT control run (penalty=0, 1 region) ====="
run_once 0   7417 flat

# far_msgs (cross-boundary, summary-only) and near_msgs (same-region fan-in).
ZFB=$(loc_at /tmp/pk4_zoned_node1.log far_msgs 1);  ZFA=$(loc_at /tmp/pk4_zoned_node1.log far_msgs 2)
ZNB=$(loc_at /tmp/pk4_zoned_node1.log near_msgs 1); ZNA=$(loc_at /tmp/pk4_zoned_node1.log near_msgs 2)
FFB=$(loc_at /tmp/pk4_flat_node1.log  far_msgs 1);  FFA=$(loc_at /tmp/pk4_flat_node1.log  far_msgs 2)
FNB=$(loc_at /tmp/pk4_flat_node1.log  near_msgs 1); FNA=$(loc_at /tmp/pk4_flat_node1.log  near_msgs 2)
: "${ZFB:=0}" "${ZFA:=0}" "${ZNB:=0}" "${ZNA:=0}" "${FFB:=0}" "${FFA:=0}" "${FNB:=0}" "${FNA:=0}"
ZONED_FAR=$(( ZFA - ZFB ));  ZONED_NEAR=$(( ZNA - ZNB ))
FLAT_FAR=$(( FFA - FFB ));    FLAT_NEAR=$(( FNA - FNB ))

echo
echo "===== node 1 (ZONED) region view ====="
grep -E "\[region\]" /tmp/pk4_zoned_node1.log
echo "===== node 1 (FLAT control) region view ====="
grep -E "\[region\]" /tmp/pk4_flat_node1.log
echo
echo "===== [fed-2cluster][live] MEASURED GATE ====="
echo "  zoned [locality] delta: far=$ZONED_FAR  near=$ZONED_NEAR  (summary-only crossing + own-region fan-in)"
echo "  flat  [locality] delta: far=$FLAT_FAR  near=$FLAT_NEAR  (no boundary -> direct fan-in to all N)"

PASS=1
# Gate (a): O(#region) bound on cross-boundary traffic. #remote_regions=1; K =
# bounded per-region summary msgs (GLOBAL Q to coordB + GLOBAL rsum back). The
# far delta must NOT scale with region-B's node count (2). K=6 absorbs cumulative
# counter jitter from background SWIM/world (plan §8.3).
REMOTE_REGIONS=1; K=6; BOUND=$(( REMOTE_REGIONS * K ))
if [ "$ZONED_FAR" -le "$BOUND" ]; then
    echo "  [live] (a) O(#region): zoned far delta $ZONED_FAR <= #remote_regions($REMOTE_REGIONS)*K($K)=$BOUND : PASS"
else
    echo "  [live] (a) O(#region): zoned far delta $ZONED_FAR > $BOUND : FAIL (cross-region scaling with N?)"; PASS=0
fi

# Gate (b): DEGENERATION FALSIFIER (two mechanical distinguishers).
#  (b1) a real boundary exists ONLY in the zoned run: zoned crosses it (far>0),
#       flat has no boundary (far==0).
if [ "$ZONED_FAR" -gt 0 ] && [ "$FLAT_FAR" -eq 0 ]; then
    echo "  [live] (b1) boundary exists: zoned far $ZONED_FAR>0, flat far $FLAT_FAR==0 : PASS"
else
    echo "  [live] (b1) boundary: zoned far $ZONED_FAR, flat far $FLAT_FAR : FAIL (no boundary formed?)"; PASS=0
fi
#  (b2) the hierarchy HIDES region-B's dense partials behind one summary, so
#       node1's same-region fan-in is SMALLER zoned than flat. If region-scoping
#       degenerated to all-to-all, node1 would collect everyone directly in both
#       runs and the two near deltas would be EQUAL -> this fails.
if [ "$ZONED_NEAR" -lt "$FLAT_NEAR" ]; then
    echo "  [live] (b2) FALSIFIER: zoned near $ZONED_NEAR < flat near $FLAT_NEAR : PASS (region-B chatter hidden behind summary)"
else
    echo "  [live] (b2) FALSIFIER: zoned near $ZONED_NEAR NOT < flat near $FLAT_NEAR : FAIL (region scoping is a no-op?)"; PASS=0
fi

# Gate (c): ONE-MATH. The hierarchy must not change the answer.
ZFP=$(fp_of /tmp/pk4_zoned_node1.log); FFP=$(fp_of /tmp/pk4_flat_node1.log)
echo "  zoned answer: ${ZFP:-<none>}"
echo "  flat  answer: ${FFP:-<none>}"
if [ -n "$ZFP" ] && [ "$ZFP" = "$FFP" ]; then
    echo "  [live] (c) ONE-MATH: answer identical zoned vs flat : PASS"
else
    echo "  [live] (c) ONE-MATH: answer differs (or missing) zoned vs flat : FAIL"; PASS=0
fi

echo
echo "===== region B (ZONED) — internal partials folded into one summary ====="
echo "node 3 (id3, coordinator) published region summary: $(grep -c 'region summary published' /tmp/pk4_zoned_node3.log)"
echo "node 4 (id4) partial stays in region B (responded):  $(grep -c 'responded to node 0' /tmp/pk4_zoned_node4.log)"
echo
if [ "$PASS" = 1 ]; then echo "[fed-2cluster][live] PASS"; else echo "[fed-2cluster][live] FAIL"; fi
echo "[demo] done. full logs in /tmp/pk4_{zoned,flat}_node{1..4}.log /tmp/pk4_*_relay.log"
[ "$PASS" = 1 ]
