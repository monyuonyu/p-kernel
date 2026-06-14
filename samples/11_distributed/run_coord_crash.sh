#!/bin/bash
# ===========================================================================
# [live] Coordinator-crash re-delegation cert  ([coord-crash])
#
#   Critique §4, reworded: "no central" really means "the coordinator role is
#   not FIXED but DETERMINISTICALLY DELEGATED to the region's minimum live
#   node-id" (arch/common/region.c: region_recompute() -> coord = min member
#   whose state==ALIVE).  What was NOT proven LIVE: the role re-delegating when
#   the coordinator CRASHES with a `kill -9`, and aggregation still converging
#   through whoever takes over.  This cert proves both with real processes and
#   a real SIGKILL, in two complementary parts:
#
#   PART A -- role re-delegation, observed directly (region.c, the invariant
#             itself).  region A = {node1(id0), node2(id1)}; node2 is the
#             requester.  Min live id = id0, so node2 sees coordinator=node0.
#             `kill -9 node1` (the coordinator).  After SWIM declares id0 DEAD,
#             node2's region_recompute() drops id0 and RE-DERIVES coordinator
#             = node1 (itself, the next min live id) -- LOCALLY, no election,
#             no central arbiter.  Observed via the `region` command flipping
#             `coordinator=node0` -> `coordinator=node1 (self)`.
#             DISEASE: without re-derivation the requester would keep pointing
#             at the dead node0 (stale coordinator) forever.
#             CURE (measured): the flip happens within SWIM's DEAD window.
#             Liveness: node2 still answers `ver` after the crash (no hang).
#
#   PART B -- aggregation reconverges through the survivor when a coordinator
#             the requester DEPENDS ON crashes mid-aggregation (dkva.c rsum
#             path).  Hub requester node1(id0) reliably has RTT to a remote
#             pair node3(id2)+node4(id3); it folds each remote region summary
#             (rsum/<rid>) into its attention.  Steady state: it folds rid=2
#             AND rid=3 ("aggregated ... 2 remote regions, 12 KV entries").
#             `kill -9 node3` (id2) MID-RUN while infers are in flight.
#             DISEASE: a central design would stall/hang or silently return a
#             wrong partial when a contributor dies.
#             CURE (measured): the requester keeps converging through the
#             SURVIVING coordinator (folds rid=3 only, "1 remote region,
#             9 KV entries") AND accounts the loss HONESTLY ("degraded (k/n)")
#             -- no hang, no silent wrong result, no central SPOF.  The
#             requester re-derived its expectation set locally (region_recompute).
#
#   Exit 0 = [coord-crash] PASS.  Honest: any stale coordinator, any hang, any
#   silent (non-degraded) loss, or a missing re-delegation FAILS loudly.
#
#   NOTE (scope, honest): a THIRD demo -- two NON-hub nodes forming one remote
#   region and the survivor re-publishing the SAME region's rsum after the min
#   dies -- is blocked by a separate, reproducible discovery-layer limitation
#   of this co-located relay harness: non-hub nodes reliably SWIM-mesh only the
#   hub (id0), so two passive peers never co-region here (see the cert report /
#   OPEN finding).  That is a discovery-completeness issue, NOT a defect in the
#   min-id re-delegation invariant, which Parts A and B prove live.
#
#   Run:   ./run_coord_crash.sh        (aarch64 native here)
#   Logs:  /tmp/cc_a_*.log  /tmp/cc_b_*.log
# ===========================================================================
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

FAIL=0
waitfor() { # waitfor <file> <pattern> <timeout_s>
    local f="$1" pat="$2" t="${3:-30}" i
    for ((i = 0; i < t * 10; i++)); do
        grep -Eq "$pat" "$f" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

# ===========================================================================
# PART A -- role re-delegation observed directly via region.c
# ===========================================================================
echo "=============================================================="
echo "[coord-crash] PART A: min-id coordinator re-delegation on crash"
echo "=============================================================="
export PKERNEL_RELAY_PORT=7451
unset PKERNEL_RTT_ZONE_SIZE PKERNEL_RTT_ZONE_PENALTY   # single region {id0,id1}
A_PIDS=(); A_N1=
FIFO_A=/tmp/cc_a_in.$$
: > /tmp/cc_a_n1.log; : > /tmp/cc_a_n2.log; : > /tmp/cc_a_relay.log
mkfifo "$FIFO_A" || exit 1
cleanup_a() { exec 8>&- 2>/dev/null; kill "${A_PIDS[@]}" 2>/dev/null; rm -f "$FIFO_A"; }

"$ROOT/relay/relay" -p 7451 -v >/tmp/cc_a_relay.log 2>&1 & A_PIDS+=($!)
sleep 1
# node1 = id0 (the coordinator-to-be-killed)
PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 "$BOOT/p-kernel" </dev/null >/tmp/cc_a_n1.log 2>&1 & A_N1=$!; A_PIDS+=($A_N1)
sleep 2
# node2 = id1 (the requester; sees coordinator=node0)
PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 "$BOOT/p-kernel" <"$FIFO_A" >/tmp/cc_a_n2.log 2>&1 & A_PIDS+=($!)
exec 8> "$FIFO_A"
sendA() { echo "$1" >&8; }

if ! waitfor /tmp/cc_a_n2.log "Type 'help' for commands" 30; then
    echo "FAIL[A]: requester never reached shell"; cleanup_a; exit 1
fi
sleep 14
sendA "region"; sleep 2

PRE_COORD0=$(grep -c "coordinator=node0" /tmp/cc_a_n2.log)
echo "[A] pre-kill: requester sees 'coordinator=node0' lines : $PRE_COORD0  (expect >0)"
if [ "$PRE_COORD0" -lt 1 ]; then
    echo "FAIL[A]: requester did not form region {id0,id1} with coordinator=node0."
    FAIL=1
fi
A_OFF=$(wc -c < /tmp/cc_a_n2.log)

echo "[A] >>> kill -9 node1 (id0, the region coordinator) <<<"
kill -9 "$A_N1" 2>/dev/null
# poll region until coordinator re-delegates to self (node1) -- the cure
sendA "region"
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do sleep 1.5; sendA "region"; done

# liveness after coordinator crash
sendA "ver"
if waitfor /tmp/cc_a_n2.log "T-Kernel core" 10; then
    echo "[A] liveness: requester still answers 'ver' after coordinator crash (no hang)."
else
    echo "FAIL[A]: requester did not answer after the crash -- hang/SPOF."
    FAIL=1
fi
sendA "exit"; sleep 1

POST_SELF=$(tail -c +$((A_OFF+1)) /tmp/cc_a_n2.log | grep -Ec "coordinator=node1 \(self\)")
DEAD_SEEN=$(grep -Ec "node 0 -> DEAD|node 0 DEAD" /tmp/cc_a_n2.log)
# stale = still naming the dead node0 as coordinator AFTER SWIM declared it DEAD.
# (region prints in the pre-DEAD window legitimately still say node0 -- id0 is
# not dead yet there; only post-DEAD stale would be a re-derivation bug.)
STALE_AFTER_DEAD=$(awk '/node 0 -> DEAD|node 0 DEAD/{d=1} d && /coordinator=node0/{c++} END{print c+0}' /tmp/cc_a_n2.log)
# the final coordinator line the requester printed must be itself (node1)
LAST_COORD=$(grep -E "\[region\] id=" /tmp/cc_a_n2.log | tail -1)
echo
echo "[A] measurement:"
echo "    SWIM declared id0 DEAD on requester           : $([ "$DEAD_SEEN" -ge 1 ] && echo yes || echo no)"
echo "    post-kill 'coordinator=node1 (self)' lines    : $POST_SELF   (cure: must be >0)"
echo "    stale 'coordinator=node0' AFTER id0 DEAD      : $STALE_AFTER_DEAD  (must be 0)"
echo "    final region line                             : $LAST_COORD"
if [ "$DEAD_SEEN" -lt 1 ]; then
    echo "FAIL[A]: SWIM never declared the crashed coordinator DEAD."
    FAIL=1
fi
if [ "$POST_SELF" -lt 1 ]; then
    echo "FAIL[A]: coordinator did NOT re-delegate to the next min id (node1) after the crash."
    FAIL=1
fi
if [ "$STALE_AFTER_DEAD" -ne 0 ]; then
    echo "FAIL[A]: requester kept naming the DEAD node0 as coordinator after it was declared DEAD (stale)."
    FAIL=1
fi
case "$LAST_COORD" in
    *"coordinator=node1 (self)"*) : ;;
    *) echo "FAIL[A]: final coordinator is not the re-delegated min id (node1)."; FAIL=1 ;;
esac
cleanup_a
sleep 1

# ===========================================================================
# PART B -- aggregation reconverges through the survivor (dkva.c rsum path)
# ===========================================================================
echo
echo "=============================================================="
echo "[coord-crash] PART B: DKVA reconverges past a crashed coordinator"
echo "=============================================================="
export PKERNEL_RELAY_PORT=7452
export PKERNEL_RTT_ZONE_SIZE=2
export PKERNEL_RTT_ZONE_PENALTY=200
B_PIDS=(); B_N3=
FIFO_B=/tmp/cc_b_in.$$
: > /tmp/cc_b_n1.log; : > /tmp/cc_b_n2.log; : > /tmp/cc_b_n3.log; : > /tmp/cc_b_n4.log; : > /tmp/cc_b_relay.log
mkfifo "$FIFO_B" || { FAIL=1; }
cleanup_b() { exec 9>&- 2>/dev/null; kill "${B_PIDS[@]}" 2>/dev/null; rm -f "$FIFO_B"; }

"$ROOT/relay/relay" -p 7452 -v >/tmp/cc_b_relay.log 2>&1 & B_PIDS+=($!)
sleep 1
# region B (zone1): node3(id2) + node4(id3); region A (zone0): hub requester id0 + id1
PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 "$BOOT/p-kernel" </dev/null >/tmp/cc_b_n2.log 2>&1 & B_PIDS+=($!)
PKERNEL_NODE_ID=3 PKERNEL_AUTONET=1 "$BOOT/p-kernel" </dev/null >/tmp/cc_b_n3.log 2>&1 & B_N3=$!; B_PIDS+=($B_N3)
PKERNEL_NODE_ID=4 PKERNEL_AUTONET=1 "$BOOT/p-kernel" </dev/null >/tmp/cc_b_n4.log 2>&1 & B_PIDS+=($!)
sleep 2
# requester = node1 (id0, the hub: reliably meshes all)
PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 "$BOOT/p-kernel" <"$FIFO_B" >/tmp/cc_b_n1.log 2>&1 & B_PIDS+=($!)
exec 9> "$FIFO_B"
sendB() { echo "$1" >&9; }

if ! waitfor /tmp/cc_b_n1.log "Type 'help' for commands" 30; then
    echo "FAIL[B]: requester never reached shell"; cleanup_b; exit 1
fi
sleep 14
sendB "region"; sleep 1
echo "[B] phase-1 (healthy): infer; expect to fold BOTH rid=2 and rid=3"
for i in 1 2 3; do sendB "infer 4$i 20 90 5"; sleep 1.6; done
sleep 1

PRE_FOLD2=$(grep -c "region summary rid=2" /tmp/cc_b_n1.log)
PRE_FOLD3=$(grep -c "region summary rid=3" /tmp/cc_b_n1.log)
PRE_2REG=$(grep -Fc "aggregated 1 region peers + 2 remote regions" /tmp/cc_b_n1.log)
echo "[B]   pre-kill folds rid=2 : $PRE_FOLD2  (expect >0)"
echo "[B]   pre-kill folds rid=3 : $PRE_FOLD3  (expect >0)"
echo "[B]   pre-kill 2-remote-region aggregations : $PRE_2REG  (expect >0)"
if [ "$PRE_FOLD2" -lt 1 ] || [ "$PRE_FOLD3" -lt 1 ] || [ "$PRE_2REG" -lt 1 ]; then
    echo "FAIL[B]: baseline did not fold both remote coordinators (rid=2 & rid=3)."
    FAIL=1
fi
B_OFF=$(wc -c < /tmp/cc_b_n1.log)

echo "[B] >>> kill -9 node3 (id2 = a coordinator the requester folds, rid=2) MID-RUN <<<"
sendB "infer 70 20 90 5"      # an aggregation in flight at the instant of death
sleep 0.15
kill -9 "$B_N3" 2>/dev/null
for i in 1 2 3 4 5 6 7 8; do sleep 1.6; sendB "infer 6$i 20 90 5"; done
sleep 1
sendB "ver"
if waitfor /tmp/cc_b_n1.log "T-Kernel core" 10; then
    echo "[B] liveness: requester still answers 'ver' after the coordinator crash (no hang)."
else
    echo "FAIL[B]: requester did not answer after the crash -- hang/SPOF."
    FAIL=1
fi
sendB "exit"; sleep 1

POST_FOLD3=$(tail -c +$((B_OFF+1)) /tmp/cc_b_n1.log | grep -Ec "region summary rid=3")
POST_FOLD2=$(tail -c +$((B_OFF+1)) /tmp/cc_b_n1.log | grep -Ec "region summary rid=2")
POST_DEGRADED=$(tail -c +$((B_OFF+1)) /tmp/cc_b_n1.log | grep -Ec "degraded \(")
POST_1REG=$(tail -c +$((B_OFF+1)) /tmp/cc_b_n1.log | grep -Fc "aggregated 1 region peers + 1 remote regions")
echo
echo "[B] measurement:"
echo "    post-kill folds rid=3 (survivor)           : $POST_FOLD3   (cure: must be >0)"
echo "    post-kill folds rid=2 (DEAD coordinator)   : $POST_FOLD2   (should drop; new aggs must not need it)"
echo "    post-kill HONEST 'degraded (k/n)' lines    : $POST_DEGRADED (must be >0: loss accounted, not silent)"
echo "    post-kill 1-remote-region reconvergences   : $POST_1REG    (must be >0: converged via survivor)"
if [ "$POST_FOLD3" -lt 1 ]; then
    echo "FAIL[B]: requester stopped folding the SURVIVING coordinator -- aggregation lost region B entirely."
    FAIL=1
fi
if [ "$POST_DEGRADED" -lt 1 ]; then
    echo "FAIL[B]: requester did NOT report the crashed contributor as degraded -- silent loss (dishonest)."
    FAIL=1
fi
if [ "$POST_1REG" -lt 1 ]; then
    echo "FAIL[B]: requester never produced a converged aggregation after the crash -- possible stall."
    FAIL=1
fi
cleanup_b

# ===========================================================================
echo
echo "===== key log lines ====="
echo "--- [A] requester region: coordinator re-delegates node0 -> node1 ---"
grep -E "\[region\] id=|node 0 .*DEAD" /tmp/cc_a_n2.log | sed 's/^/  /' | tail -10
echo "--- [B] requester DKVA: folds before/after crash + honest degraded ---"
grep -E "region summary rid=|aggregated 1 region|degraded \(" /tmp/cc_b_n1.log | sed 's/^/  /' | tail -16
echo "========================="

if [ "$FAIL" -eq 0 ]; then
    echo "[coord-crash] PASS [live] -- coordinator crash (kill -9) re-delegated to the next"
    echo "[coord-crash] min live id (A: node0->node1 via region_recompute), and distributed"
    echo "[coord-crash] aggregation reconverged through the surviving coordinator with honest"
    echo "[coord-crash] degraded accounting (B), node stayed alive -- no central SPOF."
else
    echo "[coord-crash] FAIL -- see /tmp/cc_a_*.log /tmp/cc_b_*.log"
fi
exit $FAIL
