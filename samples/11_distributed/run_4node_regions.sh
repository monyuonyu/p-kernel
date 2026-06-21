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
# GATES (all must PASS):
#   [fed-2cluster][live]           — REQUESTER-side (R0): zoned-vs-flat far/near
#       deltas on node1 (O(#region) bound + flat-control degeneration falsifier).
#   [fed-2cluster][live][responder] — RESPONDER-side (R0.1 item 1): node3 (coordB)
#       AND node4 (non-coord responder) print their OWN [locality]; a resp/<n>
#       REGION→GLOBAL degeneration manifests on the RESPONDERS' TX (node1 never
#       sees it — the R0 audit's "COVERAGE GAP"). The respglobal run arms the
#       PKERNEL_DKVA_RESP_GLOBAL=1 falsifier on the responders and the gate goes
#       RED (node4 far jumps from background-only to the dense range).
#
# Usage:   ./run_4node_regions.sh
# Watch:   /tmp/pk4_{zoned,flat,respglobal}_node{1..4}.log  /tmp/pk4_*_relay.log
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

# run_once <penalty> <port> <tag> [resp_global] : start a fresh relay+4 nodes,
#   drive node1 (the REQUESTER) AND nodes 3,4 (the region-B RESPONDERS) so all
#   three print [locality] BEFORE and AFTER node1's one inference.
#   <resp_global>=1 sets PKERNEL_DKVA_RESP_GLOBAL=1 on the RESPONDER nodes only
#   (3,4) — the R0.1 falsifier that degenerates resp/<n> REGION→GLOBAL so the
#   responder-side gate must go RED. Default 0 = production.
#
#   Why drive the responders: R0's [live] gate read ONLY node1 (the requester);
#   a resp/<n> REGION→GLOBAL degeneration manifests on the RESPONDER nodes' TX
#   (their dense per-node partials cross the boundary), which node1 never sees
#   (audit-trail "COVERAGE GAP", R0.1 item 1). So we also measure coordB (node3,
#   id3 = region-B coordinator) and node4 (id4 = a NON-coordinator responder).
#
#   Logs: /tmp/pk4_<tag>_node{1,3,4}.log are driven (FIFO); node2 stays passive.
run_once() {
    local penalty="$1" port="$2" tag="$3" resp_global="${4:-0}"
    local pids=()
    local F1=/tmp/pk4_${tag}_in1.$$ F3=/tmp/pk4_${tag}_in3.$$ F4=/tmp/pk4_${tag}_in4.$$
    rm -f "$F1" "$F3" "$F4"; mkfifo "$F1" "$F3" "$F4" || return 1
    PKERNEL_RELAY_PORT="$port" "$ROOT/relay/relay" -p "$port" -v \
        >/tmp/pk4_${tag}_relay.log 2>&1 & pids+=($!)
    sleep 1
    # node2 (id2): passive region-A peer, never driven.
    env PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 \
        PKERNEL_RELAY_PORT="$port" PKERNEL_RTT_ZONE_PENALTY="$penalty" \
        "$BOOT/p-kernel" </dev/null >/tmp/pk4_${tag}_node2.log 2>&1 & pids+=($!)
    # nodes 3 (id3, coordB) + 4 (id4, responder): driven via FIFO so they print
    # [locality]. The falsifier (resp_global=1) is applied ONLY to the responders.
    local i
    for i in 3 4; do
        env PKERNEL_NODE_ID=$i PKERNEL_AUTONET=1 \
            PKERNEL_RELAY_PORT="$port" PKERNEL_RTT_ZONE_PENALTY="$penalty" \
            PKERNEL_DKVA_RESP_GLOBAL="$resp_global" \
            "$BOOT/p-kernel" <"$(eval echo \$F$i)" >/tmp/pk4_${tag}_node$i.log 2>&1 & pids+=($!)
    done
    # requester node1 (id1): production resp scope (falsifier never on the
    # requester — the requester sees the SAME counters either way; the point is
    # the degeneration shows up on the RESPONDERS).
    env PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 \
        PKERNEL_RELAY_PORT="$port" PKERNEL_RTT_ZONE_PENALTY="$penalty" \
        "$BOOT/p-kernel" <"$F1" >/tmp/pk4_${tag}_node1.log 2>&1 & pids+=($!)
    # open all three FIFO write ends.
    exec 6>"$F1" 7>"$F3" 8>"$F4"
    sleep 2

    # Coordinated schedule (independent timers, so drive in lockstep here):
    sleep 13                              # SWIM RTT + regions settle
    echo "region" >&6                     # requester region view
    sleep 1
    echo "kdds" >&6; echo "kdds" >&7; echo "kdds" >&8    # [locality] BEFORE
    sleep 1
    echo "infer 50 20 90 5" >&6           # ONE hierarchical inference
    sleep 3
    echo "kdds" >&6; echo "kdds" >&7; echo "kdds" >&8    # [locality] AFTER
    sleep 1
    echo "exit" >&6; echo "exit" >&7; echo "exit" >&8
    sleep 1
    exec 6>&- 7>&- 8>&-
    kill "${pids[@]}" 2>/dev/null; wait 2>/dev/null
    rm -f "$F1" "$F3" "$F4"
}

# Pull the N-th (1-based) <name>= value from a node1 log's [locality] lines.
loc_at() { sed -n "s/.*\[locality\].* $2=\([0-9]*\).*/\1/p" "$1" | sed -n "${3}p"; }
# Pull node1's deterministic answer (=> class N) zoned vs flat.
fp_of()  { grep -oE "=> class [0-9]+" "$1" | tail -1; }

echo "[demo] ===== ZONED run (penalty=200, 2 regions) ====="
run_once 200 7416 zoned
echo "[demo] ===== FLAT control run (penalty=0, 1 region) ====="
run_once 0   7417 flat
echo "[demo] ===== RESP-GLOBAL falsifier run (penalty=200, resp degenerated to GLOBAL on responders) ====="
run_once 200 7418 respglobal 1

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
# bounded per-region summary msgs (GLOBAL Q to coordB + GLOBAL rsum back) PLUS a
# small allowance for cumulative background SWIM/world GLOBAL gossip that also
# crosses the boundary during the measurement window. The far delta must stay
# O(1)-bounded and must NOT scale with region-B's node count — a per-node
# degeneration would push node1 far into the dense range (~10²), not ~10¹.
# K=10 (was 6) absorbs the extra background jitter from now keeping the region-B
# responder nodes alive+polled across the whole window (they print [locality]
# for the responder-side gate below); the flat-control far==0 (b1) and the
# near-collapse (b2) remain the load-bearing degeneration falsifiers.
REMOTE_REGIONS=1; K=10; BOUND=$(( REMOTE_REGIONS * K ))
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

# ===========================================================================
# [fed-2cluster][live][responder] — RESPONDER-SIDE locality gate (R0.1 item 1)
# ===========================================================================
# R0's gate above measured ONLY node1 (the requester). A resp/<n> REGION→GLOBAL
# degeneration shows up on the RESPONDER nodes' TX — their dense per-node
# partials would cross the region boundary — which node1 never sees (audit-trail
# "COVERAGE GAP"). So here we read the responders' OWN [locality] counters:
#   node4 (id4) = a NON-coordinator responder: publishes resp/<me> (REGION) and
#       NO rsum. In production its dense partial stays in-region -> its FAR delta
#       must be 0. If resp degenerates to GLOBAL, that partial crosses the
#       boundary to region A -> node4 FAR delta jumps > 0  (the clean falsifier).
#   node3 (id3) = coordB: publishes resp/<me> (REGION, near) + ONE rsum (GLOBAL,
#       far). In production its FAR delta is bounded (summary-only) and its NEAR
#       delta > 0 (dense resp stays in-region). Under resp-GLOBAL its FAR delta
#       jumps because the dense resp now also crosses.
# We read the SAME zoned run as the requester gate for the healthy side, plus the
# respglobal run for the falsifier side. node4 (non-coord) is the load-bearing
# distinguisher; node3 corroborates.

# healthy (production resp=REGION) responder deltas — from the zoned run.
H4FB=$(loc_at /tmp/pk4_zoned_node4.log far_msgs 1);  H4FA=$(loc_at /tmp/pk4_zoned_node4.log far_msgs 2)
H4NB=$(loc_at /tmp/pk4_zoned_node4.log near_msgs 1); H4NA=$(loc_at /tmp/pk4_zoned_node4.log near_msgs 2)
H3FB=$(loc_at /tmp/pk4_zoned_node3.log far_msgs 1);  H3FA=$(loc_at /tmp/pk4_zoned_node3.log far_msgs 2)
H3NB=$(loc_at /tmp/pk4_zoned_node3.log near_msgs 1); H3NA=$(loc_at /tmp/pk4_zoned_node3.log near_msgs 2)
# falsifier (resp=GLOBAL on responders) deltas — from the respglobal run.
G4FB=$(loc_at /tmp/pk4_respglobal_node4.log far_msgs 1);  G4FA=$(loc_at /tmp/pk4_respglobal_node4.log far_msgs 2)
G3FB=$(loc_at /tmp/pk4_respglobal_node3.log far_msgs 1);  G3FA=$(loc_at /tmp/pk4_respglobal_node3.log far_msgs 2)
: "${H4FB:=0}" "${H4FA:=0}" "${H4NB:=0}" "${H4NA:=0}" "${H3FB:=0}" "${H3FA:=0}" "${H3NB:=0}" "${H3NA:=0}"
: "${G4FB:=0}" "${G4FA:=0}" "${G3FB:=0}" "${G3FA:=0}"
H4_FAR=$(( H4FA - H4FB ));  H4_NEAR=$(( H4NA - H4NB ))
H3_FAR=$(( H3FA - H3FB ));  H3_NEAR=$(( H3NA - H3NB ))
G4_FAR=$(( G4FA - G4FB ));  G3_FAR=$(( G3FA - G3FB ))

echo
echo "===== [fed-2cluster][live][responder] RESPONDER-SIDE GATE (R0.1) ====="
echo "  HEALTHY (resp=REGION):"
echo "    node4 (id4, non-coord responder) delta: far=$H4_FAR  near=$H4_NEAR"
echo "    node3 (id3, coordB)              delta: far=$H3_FAR  near=$H3_NEAR"
echo "  FALSIFIER (resp=GLOBAL on responders):"
echo "    node4 (id4) delta: far=$G4_FAR   node3 (id3) delta: far=$G3_FAR"

# Sanity: the falsifier must actually have been armed (resp=GLOBAL printed by
# the responders). Otherwise a missing-knob would make the gate vacuously green.
RG_ARMED=$(grep -c 'resp=GLOBAL(falsifier!)' /tmp/pk4_respglobal_node4.log)
RG_OFF=$(grep -c 'resp=region' /tmp/pk4_zoned_node4.log)
echo "  falsifier armed on respglobal node4 (resp=GLOBAL printed): $RG_ARMED  (expect >0)"
echo "  falsifier OFF on healthy zoned node4 (resp=region printed): $RG_OFF  (expect >0)"
if [ "$RG_ARMED" -lt 1 ] || [ "$RG_OFF" -lt 1 ]; then
    echo "  [live][responder] (arm) the falsifier knob did not toggle resp scope : FAIL"; PASS=0
fi

# (R1) HEALTHY: a non-coordinator responder publishes its dense per-node partial
#      ONLY to resp/<me> (REGION) and NO rsum. With resp REGION-scoped that dense
#      fan-in stays IN-REGION (near), so node4's NEAR delta dominates and its FAR
#      delta is just the small background GLOBAL gossip (SWIM/world beacons that
#      legitimately cross). The region-scoped invariant: node4 far << node4 near.
#      Under the falsifier the dense resp moves to far and this flips.
if [ "$H4_NEAR" -gt "$H4_FAR" ] && [ "$(( H4_FAR * 4 ))" -lt "$H4_NEAR" ]; then
    echo "  [live][responder] (R1) non-coord resp stays REGION (near): node4 near $H4_NEAR >> far $H4_FAR (4x) : PASS"
else
    echo "  [live][responder] (R1) node4 near $H4_NEAR / far $H4_FAR : FAIL (responder dense resp leaking across boundary?)"; PASS=0
fi

# (R2) HEALTHY: coordB crosses the boundary ONLY via its region summary (rsum,
#      GLOBAL) — NOT the dense per-node resp fan-in. Its NEAR delta (in-region
#      dense resp) is O(region²) and its FAR is the rsum republish. The falsifier
#      adds the DENSE resp to its far traffic on top of rsum; so coordB far must
#      JUMP under resp=GLOBAL (R2 is the coordB corroboration of R3). Measured as
#      a clean healthy-vs-falsifier separation.
# margin = half the in-region dense fan-in: cleanly above background gossip (~6)
# yet tolerant of round-robin/timing jitter (the dense resp that crosses ≈ near).
R2_MARGIN=$(( H3_NEAR / 2 )); R2_JUMP=$(( G3_FAR - H3_FAR ))
if [ "$R2_JUMP" -ge "$R2_MARGIN" ] && [ "$R2_MARGIN" -gt 0 ]; then
    echo "  [live][responder] (R2) coordB dense resp crosses under falsifier: node3 far $H3_FAR->$G3_FAR jump $R2_JUMP >= near/2 ($R2_MARGIN) : PASS"
else
    echo "  [live][responder] (R2) node3 far healthy $H3_FAR -> falsifier $G3_FAR jump $R2_JUMP (near $H3_NEAR) : FAIL (dense resp not isolated from coordB cross-traffic?)"; PASS=0
fi

# (R3) FALSIFIER (load-bearing): forcing resp=GLOBAL makes the NON-coordinator
#      responder's dense partial cross the region boundary -> node4 FAR delta
#      jumps from background-only (~$H4_FAR) to the dense range (~$G4_FAR). node4
#      has NO rsum, so its far traffic is the cleanest measure of resp scope. The
#      jump must be at least the in-region dense fan-in's worth (near) — i.e. the
#      whole dense partial set crossed. This is the mechanical distinguisher that
#      gives the responder-side gate teeth (vs R0's requester-only blindness).
if [ "$G4_FAR" -gt "$H4_FAR" ] && [ "$(( G4_FAR - H4_FAR ))" -ge "$H4_NEAR" ]; then
    echo "  [live][responder] (R3) FALSIFIER bites: resp=GLOBAL node4 far $G4_FAR vs healthy $H4_FAR (jump >= near $H4_NEAR) : PASS (dense resp degeneration caught)"
else
    echo "  [live][responder] (R3) FALSIFIER: resp=GLOBAL node4 far $G4_FAR vs healthy $H4_FAR : FAIL (gate has no teeth / dense resp did not cross)"; PASS=0
fi

echo
echo "===== region B (ZONED) — internal partials folded into one summary ====="
echo "node 3 (id3, coordinator) published region summary: $(grep -c 'region summary published' /tmp/pk4_zoned_node3.log)"
echo "node 4 (id4) partial stays in region B (responded):  $(grep -c 'responded to node 0' /tmp/pk4_zoned_node4.log)"
echo
if [ "$PASS" = 1 ]; then echo "[fed-2cluster][live] PASS"; else echo "[fed-2cluster][live] FAIL"; fi
echo "[demo] done. full logs in /tmp/pk4_{zoned,flat,respglobal}_node{1..4}.log /tmp/pk4_*_relay.log"
[ "$PASS" = 1 ]
