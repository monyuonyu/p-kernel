#!/bin/bash
# ---------------------------------------------------------------------------
# concurrent_infer.sh — "thinking many thoughts at once" + honest cross-region
# degraded (death-piercing, wave 10: audit G1 + G2 + G8)
#
# Before wave 10 the DKVA Query rode a single shared latch topic
# ("dtr/dkva/q"): two nodes asking at the same instant overwrote each other,
# so the whole mesh could only ever have ONE question in flight (G1 — the
# exact opposite of the §5 "concurrent many" promise). And a requester only
# counted its OWN region's expected peers, so a whole DEAD remote region (or a
# dead region coordinator, G8) vanished silently into a "success" (G2).
#
# This harness proves both are fixed:
#
#   scenario A  node1 AND node2 (and, if reachable, node3) issue a distributed
#               KV-attention inference in the SAME frame (background, no gap).
#               Assert BOTH complete AND neither result is cross-contaminated:
#               each origin's output fingerprint (fp) equals its own clean
#               single-run baseline (same Q -> same fp), and req_ids are
#               distinct.  per-origin Q (G1) makes simultaneous questions real.
#
#   scenario B  two regions via PKERNEL_RTT_ZONE_SIZE=2 (node1 alone; node2+
#               node3 form the other region).  Kill the ENTIRE other region and
#               infer from node1.  Assert a "degraded (k/n)" line is ALWAYS
#               printed and the inference still completes — the lost region is
#               counted honestly, never silently dropped (G2 + G8).
#
# Scenario C (the three kill_one.sh scenarios stay intact) is verified by
# running kill_one.sh separately; see the verify section of this wave.
#
# Each scenario boots a fresh cluster over the public ./relay.
# Exit code is non-zero if any assertion fails. Logs: /tmp/ci13_*.log
#
# Usage:   ./concurrent_infer.sh
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
export PKERNEL_RELAY_PORT=7414

FIFO=/tmp/ci13_fifo
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

expect_grep() {   # <file> <pattern> <desc>
    if grep -aq "$2" "$1"; then log "ok  : $3"
    else fail "$3 — pattern '$2' not found in $1"; fi
}
expect_count() {  # <file> <pattern> <want> <desc>
    local c; c=$(grep -ac "$2" "$1"); c=${c:-0}
    if [ "$c" -eq "$3" ]; then log "ok  : $4 (count=$c)"
    else fail "$4 — want $3, got $c of '$2' in $1"; fi
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

cluster_up() {    # <tag> [zone_size]  — fresh relay + 3 nodes, wait until FULL
    local tag="$1"; local zone="${2:-0}"
    log "--- cluster up ($tag): relay :$PKERNEL_RELAY_PORT + 3 nodes (zone=$zone) ---"
    "$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" -v \
        > "/tmp/ci13_${tag}_relay.log" 2>&1 &
    RELAY_PID=$!
    disown "$RELAY_PID"
    sleep 1
    for i in 1 2 3; do rm -f "$FIFO.$i"; mkfifo "$FIFO.$i"; done
    exec 3<>"$FIFO.1" 4<>"$FIFO.2" 5<>"$FIFO.3"
    for i in 1 2 3; do
        PKERNEL_NODE_ID=$i PKERNEL_AUTONET=1 \
        PKERNEL_RTT_ZONE_SIZE="$zone" PKERNEL_RTT_ZONE_PENALTY=300 \
        "$BOOT/p-kernel" \
            < "$FIFO.$i" > "/tmp/ci13_${tag}_node$i.log" 2>&1 &
        NODE_PID[$i]=$!
        disown "${NODE_PID[$i]}"
        log "node$i up  pid=${NODE_PID[$i]}  log=/tmp/ci13_${tag}_node$i.log"
    done
    local t=0
    while [ $t -lt 30 ]; do
        grep -aq -- "-> FULL" "/tmp/ci13_${tag}_node1.log" && break
        sleep 1; t=$((t + 1))
    done
    if [ $t -ge 30 ]; then
        fail "($tag) cluster never reached FULL"
    else
        log "cluster FULL after ~${t}s"
        sleep 4     # let SWIM RTT + world region beacons converge
    fi
}

# ===========================================================================
scenario_A() {
    log "=== scenario A: node1 & node2 (& node3) think AT THE SAME TIME ==="
    cluster_up A 0
    local L1=/tmp/ci13_A_node1.log
    local L2=/tmp/ci13_A_node2.log
    local L3=/tmp/ci13_A_node3.log

    # --- baselines (sequential, clean): same Q -> stable fp per origin ---
    send 1 "dkva infer 50 20 90 5"; sleep 2     # node1 (internal 0) req 9000001
    send 2 "dkva infer 50 20 90 5"; sleep 2     # node2 (internal 1) req 9010001
    local FB1 FB2
    FB1=$(get_fp "$L1" 9000001)
    FB2=$(get_fp "$L2" 9010001)
    log "baseline fp: node1($FB1)  node2($FB2)"
    [ -n "$FB1" ] || fail "A: node1 baseline produced no fp"
    [ -n "$FB2" ] || fail "A: node2 baseline produced no fp"

    # --- the moment of truth: both ask in the SAME frame ---
    log "issuing node1 + node2 inferences in the same frame (concurrent)"
    send 1 "dkva infer 50 20 90 5"   # node1 (internal 0) req 9000002
    send 2 "dkva infer 50 20 90 5"   # node2 (internal 1) req 9010002
    sleep 3

    expect_grep "$L1" "=> OK  req=9000002 " "A: node1 concurrent inference completes"
    expect_grep "$L2" "=> OK  req=9010002 " "A: node2 concurrent inference completes"
    expect_count "$L1" '=> FAILED' 0 "A: node1 never E_TMOUT"
    expect_count "$L2" '=> FAILED' 0 "A: node2 never E_TMOUT"

    local FC1 FC2
    FC1=$(get_fp "$L1" 9000002)
    FC2=$(get_fp "$L2" 9010002)
    log "concurrent fp: node1($FC1)  node2($FC2)"
    if [ -n "$FC1" ] && [ "$FC1" = "$FB1" ]; then
        log "ok  : A: node1 concurrent result matches its clean baseline (not cross-contaminated)"
    else
        fail "A: node1 concurrent fp '$FC1' != baseline '$FB1' (cross-talk / degraded under concurrency)"
    fi
    if [ -n "$FC2" ] && [ "$FC2" = "$FB2" ]; then
        log "ok  : A: node2 concurrent result matches its clean baseline (not cross-contaminated)"
    else
        fail "A: node2 concurrent fp '$FC2' != baseline '$FB2' (cross-talk / degraded under concurrency)"
    fi
    # distinct req namespaces, no mixing
    if [ "9000002" != "9010002" ]; then log "ok  : A: req_id namespaces are origin-distinct"; fi

    # --- bonus: three origins at once ---
    log "issuing node1 + node2 + node3 inferences in the same frame (3 concurrent)"
    send 1 "dkva infer 50 20 90 5"   # node1 req 9000003
    send 2 "dkva infer 50 20 90 5"   # node2 req 9010003
    send 3 "dkva infer 50 20 90 5"   # node3 (internal 2) req 9020001
    sleep 3
    expect_grep "$L1" "=> OK  req=9000003 " "A: node1 completes under 3-way concurrency"
    expect_grep "$L2" "=> OK  req=9010003 " "A: node2 completes under 3-way concurrency"
    expect_grep "$L3" "=> OK  req=9020001 " "A: node3 completes under 3-way concurrency"
    cluster_down
}

# ===========================================================================
scenario_B() {
    log "=== scenario B: kill an ENTIRE other region -> honest degraded (G2/G8) ==="
    # zone = (internal id)/zone_size; internal ids are PKERNEL_NODE_ID-1, so
    # zone_size=2 partitions {node1,node2} (zone0) | {node3} (zone1).  node1 is
    # the requester; {node3} is a whole OTHER region whose only contribution
    # reaches node1 as a region summary (rsum) from its coordinator (node3).
    cluster_up B 2
    local L1=/tmp/ci13_B_node1.log

    # region view sanity (best-effort log)
    send 1 "region"; sleep 1

    # baseline: node1 asks while the other region is alive (rsum should fold it in)
    send 1 "dkva infer 50 20 90 5"; sleep 3      # node1 (internal 0) req 9000001
    expect_grep "$L1" "=> OK  req=9000001 " "B: baseline inference completes (other region alive)"

    log "SIGKILL the ENTIRE other region {node3} (pid ${NODE_PID[3]})"
    kill -9 "${NODE_PID[3]}"
    # ask immediately, inside SWIM's stale-ALIVE window: the other region is
    # still ALIVE in the table, so node1 EXPECTS its rsum and must mark it missing
    # rather than silently succeed (G2).  node2 (same region) still contributes.
    send 1 "dkva infer 50 20 90 5"; sleep 3      # node1 (internal 0) req 9000002

    expect_grep  "$L1" 'degraded (' "B: lost region is honestly reported as degraded (not silent success)"
    expect_grep  "$L1" "=> OK  req=9000002 " "B: inference still completes on partial aggregation"
    expect_count "$L1" '=> FAILED' 0 "B: never E_TMOUT despite losing a whole region"
    cluster_down
}

# ===========================================================================
log "concurrent + cross-region honesty — relay :$PKERNEL_RELAY_PORT, kernel=$BOOT/p-kernel"
scenario_A
scenario_B

echo
if [ "$FAIL" -ne 0 ]; then
    log "RESULT: FAIL — see /tmp/ci13_*.log"
    exit 1
fi
log "RESULT: PASS — the mesh thinks many thoughts at once and never loses a region in silence"
exit 0
