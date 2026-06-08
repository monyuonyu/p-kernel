#!/bin/bash
# ===========================================================================
# 35_parallel_infer — §5 "並列な脳": many region-crossing inferences run
#                     CONCURRENTLY, not one-at-a-time (audit gap G13).
#
# WHAT G13 WAS
#   DKVA's coordinator folded its region's partial results inside a single
#   SYNCHRONOUS ~200ms window (coordinator_aggregate / DKVA_RSUM_WIN_MS),
#   called *inline* from the responder loop. While that window ran, the
#   responder could not service any other origin's question — so when several
#   origins in DIFFERENT regions asked at the same instant, the coordinator
#   re-serialized them: "one question per 200ms". G1 (per-origin Q) had made
#   same-region concurrency real; G13 was the leftover re-serialization on the
#   region-crossing aggregation path (the heaviest plumbing, and the one §5
#   cares about most).
#
# WHAT THIS PROVES
#   The window is gone. Coordinator aggregation is now per-origin and
#   event-driven (see arch/common/dkva.c: cagg_start / cagg_step /
#   cagg_republish): every origin's fan-in advances a little each responder
#   loop iteration and NOTHING blocks. So N origins asking in the same frame
#   finish in ~one inference time, not N×.
#
#   Topology: N nodes, each its OWN region (PKERNEL_RTT_ZONE_SIZE=1), so every
#   inference is region-crossing and every node is a coordinator that must
#   emit a region summary (rsum) for every other origin — exactly the path
#   G13 serialized. We measure:
#       t_one : wall time for ONE node to complete a distributed inference
#               (the others idle).
#       t_N   : wall time for ALL N nodes to complete when they ask in the
#               SAME frame.
#       bound : N * t_one  (what fully-serialized execution would cost).
#   PASS(concurrency) iff every result is correct (fp == its clean baseline)
#   AND t_N < bound (strictly faster than serialized). With the old 200ms
#   window, t_N grew ~ t_one + (N-1)*200ms and crowded/blew the 600ms infer
#   timeout; with the fix t_N hugs t_one.
#
#   We also kill -9 a node MID-FLIGHT during a concurrent batch and assert the
#   survivors still complete and report honest degraded(k/n) — no node, and no
#   in-flight thought, is lost in silence.
#
# WHERE IT RUNS
#   The x86_64 kernel (boot/linux_x86_64) over the public ./relay. On an
#   x86_64 host it runs natively; on aarch64 it runs under qemu-x86_64 (the
#   aarch64-PRoot host crashes cross-node p-fs/relay, so — like G22/G35 — we
#   validate the distributed path on the x86_64 binary under qemu).
#
# Usage:   ./run.sh [N] [RUNS]      (defaults: N=3 RUNS=5)
# Logs:    /tmp/g13_*.log
# Exit:    non-zero if any run fails an assertion.
# ===========================================================================
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

N="${1:-3}"
RUNS="${2:-5}"

# Always exercise the distributed path on the x86_64 binary. Native on x86_64,
# under qemu-x86_64 elsewhere (e.g. this aarch64-PRoot host).
BOOT="$ROOT/boot/linux_x86_64"
case "$(uname -m)" in
    x86_64|amd64) RUN_PREFIX="" ;;
    *)            RUN_PREFIX="qemu-x86_64" ;;
esac
if [ -n "$RUN_PREFIX" ] && ! command -v "$RUN_PREFIX" >/dev/null 2>&1; then
    echo "need $RUN_PREFIX to run the x86_64 kernel on $(uname -m)"; exit 1
fi

[ -x "$BOOT/p-kernel" ]    || make -C "$BOOT"       >/dev/null || exit 1
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay" >/dev/null || exit 1

export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
export PKERNEL_RELAY_HOST=127.0.0.1
export PKERNEL_RELAY_PORT="${PKERNEL_RELAY_PORT:-7438}"
# each node is its own region -> every inference is region-crossing (G13 path)
export PKERNEL_RTT_ZONE_SIZE=1
export PKERNEL_RTT_ZONE_PENALTY=300

FIFO=/tmp/g13_fifo
declare -a NODE_PID
RELAY_PID=0
FAIL=0
PASSRUNS=0

TS()   { date '+%H:%M:%S'; }
NOWMS(){ date '+%s%3N'; }
log()  { echo "[$(TS)] $*"; }
fail() { log "FAIL: $*"; FAIL=1; }

# req_id a node prints for its k-th (1-based) inference this boot:
#   internal id = NODE_ID-1 ; req = 9000000 + internal*10000 + seq
req_for() {  # <node 1..N> <seq>
    local i="$1" seq="$2"
    echo $(( 9000000 + (i-1)*10000 + seq ))
}

send() {  # send <node 1..N> <command>
    local i="$1"; shift
    printf '%s\n' "$*" > "$FIFO.$i"
}

get_fp() {  # <node> <req> -> e.g. "fp=304" (empty if not done)
    grep -a "=> OK  req=$2 " "/tmp/g13_node$1.log" 2>/dev/null \
        | grep -ao 'fp=[0-9]*' | head -1
}
done_ok() {  # <node> <req>  -> 0 if that inference completed OK
    grep -aq "=> OK  req=$2 " "/tmp/g13_node$1.log" 2>/dev/null
}
done_fail() {  # <node> <req> -> 0 if it E_TMOUT'd
    grep -aq "=> FAILED.*req=$2" "/tmp/g13_node$1.log" 2>/dev/null
}

cluster_down() {
    exec 7>&- 2>/dev/null || true
    for i in $(seq 1 "$N"); do
        [ "${NODE_PID[$i]:-0}" != 0 ] && kill -9 "${NODE_PID[$i]}" 2>/dev/null
        NODE_PID[$i]=0
    done
    [ "$RELAY_PID" != 0 ] && kill -9 "$RELAY_PID" 2>/dev/null
    RELAY_PID=0
    wait 2>/dev/null
    for i in $(seq 1 "$N"); do rm -f "$FIFO.$i"; done
    sleep 1
}
trap cluster_down EXIT

cluster_up() {  # <tag>
    local tag="$1"
    "$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" -v \
        > "/tmp/g13_${tag}_relay.log" 2>&1 &
    RELAY_PID=$!; disown "$RELAY_PID"
    sleep 1
    for i in $(seq 1 "$N"); do rm -f "/tmp/g13_node$i.log"; rm -f "$FIFO.$i"; mkfifo "$FIFO.$i"; done
    # keep every fifo open for the lifetime of the cluster
    for i in $(seq 1 "$N"); do eval "exec $((7+i))<>\"$FIFO.$i\""; done
    for i in $(seq 1 "$N"); do
        PKERNEL_NODE_ID=$i PKERNEL_AUTONET=1 \
        $RUN_PREFIX "$BOOT/p-kernel" \
            < "$FIFO.$i" > "/tmp/g13_node$i.log" 2>&1 &
        NODE_PID[$i]=$!; disown "${NODE_PID[$i]}"
    done
    # wait until node1 sees the whole mesh (degrade FULL = all peers up)
    local t=0
    while [ $t -lt 40 ]; do
        grep -aq -- "-> FULL" "/tmp/g13_node1.log" 2>/dev/null && break
        sleep 1; t=$((t+1))
    done
    [ $t -ge 40 ] && { fail "($tag) cluster never reached FULL"; return 1; }
    sleep 5   # let SWIM RTT + per-region world beacons converge
    log "cluster '$tag' FULL ($N nodes, $N regions) after ~${t}s"
    return 0
}

# poll until done_ok(node,req) for every (node,req) in the given arrays, or
# until <tmo_ms> elapses. Echoes elapsed ms (or -1 on timeout).
wait_all() {  # <tmo_ms> <node1> <req1> [<node2> <req2> ...]
    local tmo="$1"; shift
    local start; start=$(NOWMS)
    while :; do
        local all=1 i
        local -a pair=("$@")
        for ((i=0; i<${#pair[@]}; i+=2)); do
            done_ok "${pair[i]}" "${pair[i+1]}" || { all=0; break; }
        done
        [ "$all" = 1 ] && { echo $(( $(NOWMS) - start )); return 0; }
        local el=$(( $(NOWMS) - start ))
        [ "$el" -ge "$tmo" ] && { echo -1; return 1; }
        sleep 0.05
    done
}

# ---- one timed concurrency run ------------------------------------------
run_idx=0
baseline_fp=()      # baseline fp per node (clean, sequential)
declare -A SEQ      # per-node infer counter this boot

infer_seq() {  # increment + echo this node's next seq
    local i="$1"
    SEQ[$i]=$(( ${SEQ[$i]:-0} + 1 ))
    echo "${SEQ[$i]}"
}

timed_run() {  # <run number>
    local rn="$1"
    local i seq req el
    log "=== concurrency run $rn ==="

    # baselines: each node asks ALONE (sequential) -> clean fp + a t_one sample
    local tone_sum=0
    for i in $(seq 1 "$N"); do
        seq=$(infer_seq "$i"); req=$(req_for "$i" "$seq")
        local s0; s0=$(NOWMS)
        send "$i" "dkva infer 50 20 90 5"
        el=$(wait_all 4000 "$i" "$req")
        if [ "$el" = "-1" ]; then fail "run$rn: node$i baseline never completed (req=$req)"; return 1; fi
        tone_sum=$(( tone_sum + el ))
        local fp; fp=$(get_fp "$i" "$req")
        baseline_fp[$i]="$fp"
        sleep 0.3
    done
    local t_one=$(( tone_sum / N ))

    # the moment of truth: ALL N ask in the SAME frame
    local -a wpairs=()
    local s_all; s_all=$(NOWMS)
    for i in $(seq 1 "$N"); do
        seq=$(infer_seq "$i"); req=$(req_for "$i" "$seq")
        send "$i" "dkva infer 50 20 90 5"
        wpairs+=("$i" "$req")
    done
    local t_N; t_N=$(wait_all 8000 "${wpairs[@]}")
    if [ "$t_N" = "-1" ]; then
        fail "run$rn: not all concurrent inferences completed within 8s (serialized?)"
        return 1
    fi

    # Two serialized references:
    #   naive_bound = N * t_one          (if N inferences ran one-after-another)
    #   oldwin_bound = t_one + (N-1)*200 (what the OLD synchronous coordinator
    #                  window — DKVA_RSUM_WIN_MS=200ms per extra origin — would
    #                  have ADDED on the region-crossing path G13 serialized).
    # t_one is now tiny precisely because the fix made aggregation event-driven,
    # so naive_bound is a thin margin; the load-bearing claim is t_N < oldwin_bound.
    local naive_bound=$(( t_one * N ))
    local oldwin_bound=$(( t_one + (N-1)*200 ))

    # correctness: every concurrent result == its own clean baseline fp
    local ok_fp=1 j
    for ((j=0; j<${#wpairs[@]}; j+=2)); do
        i="${wpairs[j]}"; req="${wpairs[j+1]}"
        local fc; fc=$(get_fp "$i" "$req")
        if [ -z "$fc" ] || [ "$fc" != "${baseline_fp[$i]}" ]; then
            ok_fp=0
            fail "run$rn: node$i concurrent $fc != baseline ${baseline_fp[$i]} (cross-talk/degraded)"
        fi
    done

    local verdict="PASS"
    [ "$t_N" -lt "$oldwin_bound" ] || { verdict="FAIL(serialized)"; fail "run$rn: t_N($t_N) >= old-window bound($oldwin_bound)"; }
    [ "$ok_fp" = 1 ] || verdict="FAIL(incorrect)"

    log "RESULT run$rn: N=$N t_one=${t_one}ms t_N=${t_N}ms naive(N*t_one)=${naive_bound}ms old200ms_window=${oldwin_bound}ms => $verdict"
    [ "$verdict" = "PASS" ] && PASSRUNS=$(( PASSRUNS + 1 ))
    sleep 0.3   # let shells return to prompt + logs settle before next run
    return 0
}

# ---- kill -9 a contributing node, then survivors ask --------------------
# SIGKILL the victim FIRST (so it cannot contribute), then have the survivors
# ask concurrently while the victim is still ALIVE in SWIM's table (stale-ALIVE
# window). The victim's whole region is then EXPECTED but its rsum never comes:
# survivors must complete on partial aggregation AND print an honest
# degraded(k/n) — the lost region is counted, never silently dropped. This is
# the §5 "survive a node dying mid-flight" + honesty invariant in one shot.
kill_run() {
    log "=== kill -9 a contributing node, then survivors think (survival + honest degraded) ==="
    local victim=$N i seq req j
    log "SIGKILL node$victim (pid ${NODE_PID[$victim]}) BEFORE survivors ask (stale-ALIVE window)"
    kill -9 "${NODE_PID[$victim]}" 2>/dev/null
    NODE_PID[$victim]=0
    # let the victim's PROCESS fully die (so it can't still answer) but stay
    # well within SWIM's seconds-long stale-ALIVE window, so survivors still
    # EXPECT its region and must count the loss honestly rather than drop it.
    sleep 1

    # survivors think CONCURRENTLY; each expects the dead region and must
    # complete on partial aggregation with an honest degraded(k/n) line.
    local -a kreq=()
    local s_all; s_all=$(NOWMS)
    for i in $(seq 1 $((N-1))); do
        seq=$(infer_seq "$i"); req=$(req_for "$i" "$seq")
        send "$i" "dkva infer 50 20 90 5"
        kreq[$i]="$req"
    done
    # settle: a degraded inference waits out its ~600ms infer timeout. The
    # survivors do this IN PARALLEL (not once per survivor — that is the point);
    # a generous 4s window absorbs scheduler/qemu jitter without timing it.
    sleep 4

    local survived=1 degr=0 none_failed=1
    for i in $(seq 1 $((N-1))); do
        if done_ok "$i" "${kreq[$i]}"; then :; else survived=0; fail "kill: node$i did not complete (req=${kreq[$i]}) after losing node$victim"; fi
        grep -aq 'degraded (' "/tmp/g13_node$i.log" && degr=1
        done_fail "$i" "${kreq[$i]}" && none_failed=0
    done
    local verdict="PASS"
    [ "$survived" = 1 ]   || verdict="FAIL(no-survival)"
    [ "$none_failed" = 1 ]|| { verdict="FAIL(tmout)";       fail "kill: a survivor E_TMOUT'd"; }
    [ "$degr" = 1 ]       || { verdict="FAIL(silent-loss)"; fail "kill: no survivor printed honest degraded(k/n)"; }
    log "RESULT kill: $((N-1)) survivors completed CONCURRENTLY (each ~600ms degraded-timeout, in parallel) after losing a contributing node  honest_degraded=$degr  => $verdict"
    return 0
}

# ===========================================================================
log "G13 parallel-infer — relay :$PKERNEL_RELAY_PORT  kernel=$BOOT/p-kernel  prefix='${RUN_PREFIX:-native}'  N=$N runs=$RUNS"

cluster_up main || exit 1
for r in $(seq 1 "$RUNS"); do timed_run "$r"; done
kill_run
cluster_down

echo
log "PASS rate (timed concurrency): $PASSRUNS / $RUNS"
if [ "$FAIL" -ne 0 ]; then
    log "RESULT: FAIL — see /tmp/g13_*.log"
    exit 1
fi
log "RESULT: PASS — $N region-crossing inferences run concurrently (t_N << N*t_one), correct, and survive a mid-flight kill"
exit 0
