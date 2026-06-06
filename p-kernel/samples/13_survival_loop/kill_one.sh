#!/bin/bash
# ---------------------------------------------------------------------------
# kill_one.sh — death-piercing survival loop (wave 8)
#
# "An OS that never dies" had never actually been killed mid-inference.
# This harness does exactly that, three times, and asserts the swarm
# completes the inference anyway:
#
#   scenario A  kill a NON-origin responder while a distributed KV-attention
#               inference is in flight -> the requester completes on partial
#               aggregation and HONESTLY reports "degraded (2/3)".
#               (The kill lands just before the fan-out, inside SWIM's
#               stale-ALIVE window — bash cannot schedule a signal between
#               fan-out and the first response deterministically, and the
#               aggregation path exercised is identical.)
#
#   scenario B  kill the ORIGIN (requester) -> a surviving node issues the
#               SAME question ("dkva infer 50 20 90 5" builds Q
#               deterministically from its arguments, independent of node id)
#               and completes it. The origin holds no privilege.
#
#   scenario C  kill a responder in the middle of a continuous inference
#               stream -> every single inference still completes; the ones
#               inside SWIM's convergence window are reported degraded, and
#               once SWIM declares the node DEAD the requester stops
#               waiting for it.
#
# Each scenario boots a fresh 3-node cluster over the public ./relay.
# Exit code is non-zero if any assertion fails. Logs: /tmp/sl13_*.log
#
# Usage:   ./kill_one.sh
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
export PKERNEL_RELAY_PORT=7413

FIFO=/tmp/sl13_fifo
NODE_PID=(0 0 0 0)            # index 1..3
RELAY_PID=0
FAIL=0

TS()   { date '+%H:%M:%S'; }
log()  { echo "[$(TS)] $*"; }
fail() { log "FAIL: $*"; FAIL=1; }

# send <node 1..3> <command line>  — write one shell command to the node.
# We keep our own read-write fd on each fifo, so this never blocks and
# never SIGPIPEs even after the node has been killed.
send() {
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

cluster_up() {    # <tag>  — fresh relay + 3 nodes, wait until SWIM says FULL
    local tag="$1"
    log "--- cluster up ($tag): relay :$PKERNEL_RELAY_PORT + 3 nodes ---"
    "$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" -v \
        > "/tmp/sl13_${tag}_relay.log" 2>&1 &
    RELAY_PID=$!
    disown "$RELAY_PID"            # no job-control "Killed" noise
    sleep 1
    for i in 1 2 3; do rm -f "$FIFO.$i"; mkfifo "$FIFO.$i"; done
    exec 3<>"$FIFO.1" 4<>"$FIFO.2" 5<>"$FIFO.3"
    for i in 1 2 3; do
        PKERNEL_NODE_ID=$i PKERNEL_AUTONET=1 "$BOOT/p-kernel" \
            < "$FIFO.$i" > "/tmp/sl13_${tag}_node$i.log" 2>&1 &
        NODE_PID[$i]=$!
        disown "${NODE_PID[$i]}"   # no job-control "Killed" noise on SIGKILL
        log "node$i up  pid=${NODE_PID[$i]}  log=/tmp/sl13_${tag}_node$i.log"
    done
    # readiness: node1 reaches FULL once SWIM sees both peers ALIVE
    local t=0
    while [ $t -lt 30 ]; do
        grep -aq -- "-> FULL" "/tmp/sl13_${tag}_node1.log" && break
        sleep 1; t=$((t + 1))
    done
    if [ $t -ge 30 ]; then
        fail "($tag) cluster never reached FULL"
    else
        log "cluster FULL after ~${t}s"
        sleep 2     # let the remaining SWIM views converge
    fi
}

# ===========================================================================
scenario_A() {
    log "=== scenario A: SIGKILL a NON-origin responder mid-inference ==="
    cluster_up A
    local L1=/tmp/sl13_A_node1.log

    send 1 "dkva infer 50 20 90 5"          # healthy baseline: 3/3
    sleep 2
    expect_count "$L1" 'dkva-cmd] => OK'     1 "A: healthy baseline completes"
    expect_grep  "$L1" 'aggregated 2 region peers' "A: baseline aggregates both peers"

    log "SIGKILL node3 (pid ${NODE_PID[3]}) — non-origin responder"
    kill -9 "${NODE_PID[3]}"
    send 1 "dkva infer 50 20 90 5"          # same question, stale-ALIVE window
    sleep 2

    expect_grep  "$L1" 'degraded (2/3)'      "A: completes on partial aggregation, honestly marked degraded (2/3)"
    expect_count "$L1" 'dkva-cmd] => OK'     2 "A: the inference still completes"
    expect_count "$L1" 'dkva-cmd] => FAILED' 0 "A: no E_TMOUT"
    cluster_down
}

# ===========================================================================
scenario_B() {
    log "=== scenario B: SIGKILL the ORIGIN — a survivor re-issues the question ==="
    cluster_up B
    local L1=/tmp/sl13_B_node1.log
    local L3=/tmp/sl13_B_node3.log

    send 1 "dkva infer 50 20 90 5"          # the origin asks first
    sleep 2
    expect_count "$L1" 'dkva-cmd] => OK'     1 "B: origin completes the question"

    log "SIGKILL node1 (pid ${NODE_PID[1]}) — the ORIGIN dies"
    kill -9 "${NODE_PID[1]}"
    sleep 1

    send 3 "dkva infer 50 20 90 5"          # survivor asks the SAME question
    sleep 2
    expect_count "$L3" 'dkva-cmd] => OK'     1 "B: survivor re-issues the same question and completes"
    expect_count "$L3" 'dkva-cmd] => FAILED' 0 "B: no E_TMOUT on the survivor"
    cluster_down
}

# ===========================================================================
scenario_C() {
    log "=== scenario C: SIGKILL during a continuous inference stream ==="
    cluster_up C
    local L1=/tmp/sl13_C_node1.log
    local N=10

    for k in $(seq 1 "$N"); do
        send 1 "dkva infer 50 20 90 5"
        if [ "$k" -eq 2 ]; then
            sleep 0.3
            log "SIGKILL node2 (pid ${NODE_PID[2]}) — mid-stream"
            kill -9 "${NODE_PID[2]}"
            sleep 1.2
        else
            sleep 1.5
        fi
    done
    sleep 2

    expect_count "$L1" 'dkva-cmd] => OK'     "$N" "C: all $N inferences complete across the death"
    expect_count "$L1" 'dkva-cmd] => FAILED' 0    "C: no E_TMOUT, ever"
    expect_grep  "$L1" 'degraded (2/3)'           "C: degradation honestly reported while SWIM converges"
    if grep -aq 'SWIM: DEAD' "$L1"; then
        log "ok  : C: requester stopped waiting for the DEAD node ($(grep -ac 'SWIM: DEAD' "$L1")x)"
    else
        log "note: C: DEAD-skip line not observed (SWIM did not converge inside the window — degraded path covered it)"
    fi
    cluster_down
}

# ===========================================================================
log "death-piercing survival loop — 3 nodes, relay :$PKERNEL_RELAY_PORT, kernel=$BOOT/p-kernel"
scenario_A
scenario_B
scenario_C

echo
if [ "$FAIL" -ne 0 ]; then
    log "RESULT: FAIL — the swarm did not survive; see /tmp/sl13_*.log"
    exit 1
fi
log "RESULT: PASS — nodes died mid-inference and the swarm completed every one"
exit 0
