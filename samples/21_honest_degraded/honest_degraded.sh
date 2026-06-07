#!/bin/bash
# ---------------------------------------------------------------------------
# honest_degraded.sh — degraded(k/n) honesty must NOT depend on gossip
# freshness (death-piercing, wave 12: audit G12 / I4 / I16)
#
# Background (philosophy-gap-audit-2.md G12 🔴):
#   Wave 10 (G2/G8) made the requester count a whole missing remote region as
#   "degraded (k/n)" instead of silently succeeding.  But it built the remote
#   expectation set rc_expect[] from *world-gossip* region_ids, and when a
#   coordinator beacon had not yet arrived it treated EACH remote node as its
#   own region (conservative over-count).  For a remote region with SEVERAL
#   members that means: before the coordinator's region beacon converges, every
#   member is tallied as a separate expected region (rc_cnt0 inflated), yet only
#   the ONE coordinator emits an rsum — so rc_got < rc_cnt0 and the requester
#   prints a FALSE "degraded (2/3)" even though every contribution was actually
#   folded.  The number itself lied, and the lie was a function of gossip
#   timing.  concurrent_infer.sh hides this by sleeping 4s for beacons to
#   converge before measuring (its remote region is a single node, so it never
#   trips the hole anyway).
#
# This harness trips the hole on purpose and proves it is closed:
#
#   topology (zone_size=2 over 3 nodes; zone = internal_id/2):
#       node1 (id0) + node2 (id1)  -> zone0  (a MULTI-MEMBER remote region;
#                                             coordinator = node1, lowest id)
#       node3 (id2)                -> zone1  (the requester, alone)
#     node3 issues a distributed KV-attention inference.  Its only remote
#     contribution is ONE region summary (rsum) from node1, which already folds
#     node2's partial.  The TRUE remote-region count seen by node3 is 1.
#
#   gossip is held UNCONVERGED on purpose: PKERNEL_WORLD_BEACON_HOLD_MS
#   suppresses every node's world self-beacon for the whole test window, so
#   node3 can NEVER read node1/node2's region_id from gossip.  We do NOT sleep
#   to let world beacons converge (that 4s sleep is exactly what masks G12).
#   We do let SWIM RTT settle — SWIM is local (ping RTT), not the gossip whose
#   freshness honesty must not depend on; that is the whole point.
#
#   scenario UNCONVERGED  infer from node3 while world-gossip is held.
#       Assert: (a) NO over-counted/false degraded — the inflated "(2/3)" the
#               old code printed must NOT appear;
#               (b) uncertainty is made EXPLICIT — a "... uncertain ..." line
#               appears (§10 "古さ・不完全さの明示"); the result is not a
#               silent confirmed success while a remote node is unconfirmed;
#               (c) the inference completes (=> OK), and its fingerprint equals
#               the fully-converged baseline (proves all contributions were
#               folded — so any bare "degraded" would have been a lie).
#
#   scenario CONVERGED    same topology, gossip allowed to converge (no hold).
#       Assert: healthy run — completes, NO degraded, NO uncertain, and yields
#               the baseline fingerprint.  (Regression guard: the fix must not
#               change the converged path.)
#
# Each scenario boots a fresh cluster over the public ./relay.
# Exit code is non-zero if any assertion fails. Logs: /tmp/hd21_*.log
#
# Usage:   ./honest_degraded.sh
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
export PKERNEL_RELAY_PORT=7421

FIFO=/tmp/hd21_fifo
NODE_PID=(0 0 0 0)            # index 1..3
RELAY_PID=0
FAIL=0

TS()   { date '+%H:%M:%S'; }
log()  { echo "[$(TS)] $*"; }
fail() { log "FAIL: $*"; FAIL=1; }

send() {                       # send <node 1..3> <command line>
    local i="$1"; shift
    log "node$i <- '$*'"
    printf '%s\n' "$*" > "$FIFO.$i"
}

expect_grep()  {   # <file> <pattern> <desc>
    if grep -aq "$2" "$1"; then log "ok  : $3"
    else fail "$3 — pattern '$2' not found in $1"; fi
}
refute_grep()  {   # <file> <pattern> <desc>  (assert pattern is ABSENT)
    if grep -aq "$2" "$1"; then fail "$3 — unexpected pattern '$2' present in $1"
    else log "ok  : $3"; fi
}

# fp for a given req_id from a node log: "...=> OK  req=<R>  fp=<F>"
get_fp() {        # <file> <req>
    grep -a "=> OK  req=$2 " "$1" | grep -ao 'fp=[0-9]*' | head -1
}

cluster_down() {
    exec 3>&- 4>&- 5>&- 2>/dev/null || true
    for i in 1 2 3; do
        [ "${NODE_PID[$i]}" != 0 ] && kill -9 "${NODE_PID[$i]}" 2>/dev/null
        NODE_PID[$i]=0
    done
    [ "$RELAY_PID" != 0 ] && kill -9 "$RELAY_PID" 2>/dev/null
    RELAY_PID=0
    wait 2>/dev/null
    rm -f "$FIFO".{1,2,3}
    sleep 1
}
trap cluster_down EXIT

cluster_up() {    # <tag> <hold_ms>  — fresh relay + 3 nodes (zone_size=2), wait FULL
    local tag="$1"; local hold="$2"
    log "--- cluster up ($tag): relay :$PKERNEL_RELAY_PORT + 3 nodes (zone=2, beacon_hold=${hold}ms) ---"
    "$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" -v \
        > "/tmp/hd21_${tag}_relay.log" 2>&1 &
    RELAY_PID=$!
    disown "$RELAY_PID"
    sleep 1
    for i in 1 2 3; do rm -f "$FIFO.$i"; mkfifo "$FIFO.$i"; done
    exec 3<>"$FIFO.1" 4<>"$FIFO.2" 5<>"$FIFO.3"
    for i in 1 2 3; do
        PKERNEL_NODE_ID=$i PKERNEL_AUTONET=1 \
        PKERNEL_RTT_ZONE_SIZE=2 PKERNEL_RTT_ZONE_PENALTY=300 \
        PKERNEL_WORLD_BEACON_HOLD_MS="$hold" \
        "$BOOT/p-kernel" \
            < "$FIFO.$i" > "/tmp/hd21_${tag}_node$i.log" 2>&1 &
        NODE_PID[$i]=$!
        disown "${NODE_PID[$i]}"
        log "node$i up  pid=${NODE_PID[$i]}  log=/tmp/hd21_${tag}_node$i.log"
    done
    local t=0
    while [ $t -lt 30 ]; do
        grep -aq -- "-> FULL" "/tmp/hd21_${tag}_node3.log" && break
        sleep 1; t=$((t + 1))
    done
    if [ $t -ge 30 ]; then
        fail "($tag) cluster never reached FULL"
    else
        log "cluster FULL after ~${t}s"
        # Let SWIM RTT settle so regions form (node3 sees node1/node2 as a
        # remote zone).  We deliberately do NOT wait for world beacons — they
        # are held (UNCONVERGED) or allowed to flow (CONVERGED) by $hold.
        sleep 5
    fi
}

# req_id for node3 (internal id 2): 9000000 + 2*10000 + seq
REQ_BASE=9020000

# ===========================================================================
# Baseline: a fully converged cluster (no hold).  This is the "truth": the
# fingerprint node3 must reproduce, and it must run healthy (no degraded).
# ===========================================================================
FP_BASE=""
scenario_converged() {
    log "=== scenario CONVERGED: gossip allowed to converge (regression guard) ==="
    cluster_up converged 0
    local L3=/tmp/hd21_converged_node3.log
    # extra settle for world region beacons to converge (this is what the old
    # concurrent_infer.sh leans on; here it must yield a CLEAN run)
    sleep 5
    send 3 "region"; sleep 1
    send 3 "dkva infer 50 20 90 5"; sleep 4    # node3 req $((REQ_BASE+1))
    local R=$((REQ_BASE + 1))
    expect_grep "$L3" "=> OK  req=$R " "CONVERGED: inference completes"
    refute_grep "$L3" "degraded ("       "CONVERGED: healthy run prints NO degraded"
    refute_grep "$L3" "uncertain"        "CONVERGED: no uncertainty once gossip converged"
    FP_BASE=$(get_fp "$L3" "$R")
    log "baseline fp (converged) = $FP_BASE"
    [ -n "$FP_BASE" ] || fail "CONVERGED: produced no fingerprint"
    cluster_down
}

# ===========================================================================
# The G12 hole: gossip held unconverged, multi-member remote region.
# ===========================================================================
scenario_unconverged() {
    log "=== scenario UNCONVERGED: world-gossip held; multi-member remote region (G12) ==="
    cluster_up unconverged 30000
    local L3=/tmp/hd21_unconverged_node3.log

    # region view sanity (best-effort): node3 should see itself alone, with
    # node1/node2 as a remote zone (via SWIM RTT), but NOT know their region
    # from gossip (held).
    send 3 "region"; sleep 1

    send 3 "dkva infer 50 20 90 5"; sleep 5    # node3 req $((REQ_BASE+1))
    local R=$((REQ_BASE + 1))

    # (c) completes
    expect_grep "$L3" "=> OK  req=$R " "UNCONVERGED: inference completes despite unconverged gossip"
    expect_grep "$L3" '=> OK  req=' "UNCONVERGED: never E_TMOUT"

    # (a) NO over-counted/false degraded.  The old code inflated the denominator
    #     by tallying node2 as its own region -> "(2/3)".  With node3 alone +
    #     one true remote region (node1 folding node2), the denominator must
    #     NEVER be 3.  Assert the false "(2/3" string is absent.
    refute_grep "$L3" 'degraded (2/3'  "UNCONVERGED: no over-counted false degraded (old (2/3) gone)"
    refute_grep "$L3" 'degraded (1/3'  "UNCONVERGED: denominator not inflated to 3"

    # (b) uncertainty made EXPLICIT (§10): the result must not pose as a clean
    #     confirmed success while a remote node is unconfirmed by gossip.
    expect_grep "$L3" 'uncertain'      "UNCONVERGED: uncertainty is explicitly reported"
    expect_grep "$L3" 'provisional'    "UNCONVERGED: degraded count flagged provisional (gossip unconverged)"

    # (c) fingerprint equals the converged baseline -> all contributions were
    #     actually folded, so any bare 'degraded (missing contribution)' would
    #     have been a lie.  The honest report says 'uncertain', not 'missing'.
    local FP_U
    FP_U=$(get_fp "$L3" "$R")
    log "unconverged fp = $FP_U   baseline fp = $FP_BASE"
    if [ -n "$FP_U" ] && [ -n "$FP_BASE" ] && [ "$FP_U" = "$FP_BASE" ]; then
        log "ok  : UNCONVERGED: fingerprint matches converged baseline (all contributions folded; no real loss)"
    else
        fail "UNCONVERGED: fp '$FP_U' != baseline '$FP_BASE' (contributions actually lost?)"
    fi

    log "--- raw honesty line(s) from node3 (the proof) ---"
    grep -a 'degraded\|uncertain' "$L3" || true
    cluster_down
}

# ===========================================================================
log "honest degraded under unconverged gossip — relay :$PKERNEL_RELAY_PORT, kernel=$BOOT/p-kernel"
scenario_converged
scenario_unconverged

echo
if [ "$FAIL" -ne 0 ]; then
    log "RESULT: FAIL — see /tmp/hd21_*.log"
    exit 1
fi
log "RESULT: PASS — degraded(k/n) tells the truth even when gossip has not converged (G12 closed)"
exit 0
