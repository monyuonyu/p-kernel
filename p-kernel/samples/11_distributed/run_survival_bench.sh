#!/bin/bash
# ---------------------------------------------------------------------------
# run_survival_bench.sh — SURVIVAL benchmark: "kill K nodes and the cluster
# stays alive AND stays smart" as numbers, not as a slogan.
#
# Closes the remaining half of assessment_structural_gaps.md hole (6):
# "more nodes = smarter / fewer nodes = more resilient" had no benchmark.
# This harness measures the resilience axis end-to-end on real processes:
#
#   Phase 1 (learn + propagate)
#     N so_node processes on one v2 relay. Node 1 runs `dtr train` (real
#     SGD, analytic backprop) and `dtr save` (weights -> versioned p-fs
#     object "dtr/weights"). Every other node pulls the blob via p-fs P1
#     chunk replication + P2 ref gossip (`dtr load`) and must then report
#     TRAINED accuracy on the held-out split (`dtr eval`).
#     ASSERT: all N nodes reach trained accuracy.
#
#   Phase 2 (massacre + survival)
#     SIGKILL K nodes (N=4 -> K=1, 6..7 -> K=2, >=8 -> K=3; override with
#     K=...). Wait for SWIM to converge, then on the survivors:
#     ASSERT: `nodes`/`world` show exactly N-K live + K DEAD
#     ASSERT: every survivor still evals at trained accuracy
#     ASSERT: `pfs ls` still lists the 2560 B weight blob (data survival)
#     ASSERT: kdds pub/sub still works (rgnpub fanout >= 1, and a full
#             distributed `infer` round-trip returns a class)
#
#   Phase 3 (return)
#     Restart ONE killed node (same node id, fresh process = a new
#     individual with untrained weights). It must rejoin the SWIM mesh,
#     re-fetch "dtr/weights" through p-fs gossip, and eval at trained
#     accuracy — the new individual inherits the flock's memory.
#     ASSERT: rejoin observed by a survivor; eval goes from untrained
#             (~26.7%) to trained (>= ACC_MIN) after `dtr load`.
#
# Output: a markdown summary table on stdout (phase / live nodes /
# held-out accuracy / data survival / elapsed seconds). Exit nonzero if
# any assertion fails.
#
# Usage:
#   ./run_survival_bench.sh          # default N=4, K=1
#   N=8 ./run_survival_bench.sh      # N=8, K=3
#   N=6 K=2 ./run_survival_bench.sh
#
# Tunables:
#   N        cluster size (2..32), default 4
#   K        nodes to kill, default: N>=8 -> 3, N>=6 -> 2, else 1
#   SETTLE   seconds for SWIM + regions to settle after boot (default 12)
#   PORT     relay port (default 27710)
#   ACC_MIN  held-out accuracy (%) that counts as "trained" (default 90.0;
#            the model reaches ~95% train / 100% held-out, untrained ~26.7%)
#
# Watch:   /tmp/pksb_node{1..N}.log  /tmp/pksb_relay.log
# Driving: each node's stdin is a FIFO so phases are sequenced on observed
#          log output, not blind sleeps.
# ---------------------------------------------------------------------------
set -u

N="${N:-4}"
PORT="${PORT:-27710}"
SETTLE="${SETTLE:-12}"
ACC_MIN="${ACC_MIN:-90.0}"

if [ -z "${K:-}" ]; then
    if   [ "$N" -ge 8 ]; then K=3
    elif [ "$N" -ge 6 ]; then K=2
    else                      K=1; fi
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BOOT="$ROOT/boot/linux"          # so_node + libpkernel.so (aarch64 host)

if [ "$N" -lt 2 ] || [ "$N" -gt 32 ]; then
    echo "N must be in 2..32 (DNODE_MAX=32); got N=$N" >&2; exit 2
fi
if [ "$K" -lt 1 ] || [ "$K" -ge "$N" ]; then
    echo "K must be in 1..N-1; got K=$K (N=$N)" >&2; exit 2
fi

# --- build if needed --------------------------------------------------------
[ -x "$BOOT/so_node" ]       || make -C "$BOOT" so_node >/dev/null || exit 1
[ -f "$BOOT/libpkernel.so" ] || make -C "$BOOT" so_node >/dev/null || exit 1
[ -x "$ROOT/relay/relay" ]   || make -C "$ROOT/relay"   >/dev/null || exit 1

export PKERNEL_RELAY_KEY=5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b
export PKERNEL_RELAY_HOST=127.0.0.1
export PKERNEL_RELAY_PORT="$PORT"
unset PKERNEL_RTT_ZONE_SIZE PKERNEL_RTT_ZONE_PENALTY 2>/dev/null || true

LOGPFX=/tmp/pksb
FIFODIR="$(mktemp -d /tmp/pksb_fifo.XXXXXX)"
rm -f "${LOGPFX}_node"*.log "${LOGPFX}_relay.log" 2>/dev/null

RELAY_PID=
declare -a NODE_PID            # NODE_PID[id] = pid (empty if not running)

cleanup() {
    # kill by explicit PID only (never pkill -f: it has matched our own
    # shell before)
    for id in $(seq 1 "$N"); do
        p="${NODE_PID[$id]:-}"
        [ -n "$p" ] && kill -9 "$p" 2>/dev/null
    done
    [ -n "$RELAY_PID" ] && kill "$RELAY_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$FIFODIR" 2>/dev/null
}
trap cleanup EXIT

# --- tiny driver toolkit -----------------------------------------------------
nodelog() { echo "${LOGPFX}_node$1.log"; }

start_node() {                  # start_node <id>
    local id="$1" fifo="$FIFODIR/in$id" fd=$((100 + $1))
    [ -p "$fifo" ] || mkfifo "$fifo"
    # hold the FIFO open read-write from the harness: the node's open()
    # for reading never blocks and never sees EOF between commands.
    eval "exec $fd<>'$fifo'"
    ( cd "$BOOT" && \
      PKERNEL_NODE_ID=$id PKERNEL_AUTONET=1 LD_LIBRARY_PATH=. \
      exec ./so_node ) <"$fifo" >>"$(nodelog "$id")" 2>&1 &
    NODE_PID[$id]=$!
}

send() {                        # send <id> <command...>
    local id="$1"; shift
    eval "printf '%s\n' \"\$*\" >&$((100 + id))"
}

mark() { wc -l <"$(nodelog "$1")"; }          # current line count of a log

slice() {                       # slice <id> <from_mark>  -> stdout
    tail -n "+$(( $2 + 1 ))" "$(nodelog "$1")" 2>/dev/null
}

wait_log() {                    # wait_log <id> <from_mark> <regex> <timeout_s>
    local id="$1" from="$2" re="$3" to="$4" i=0
    while [ "$i" -lt $(( to * 2 )) ]; do
        slice "$id" "$from" | grep -qE "$re" && return 0
        sleep 0.5; i=$((i + 1))
    done
    return 1
}

held_out_acc() {                # held_out_acc <id> <from_mark> -> "97.3" or ""
    slice "$1" "$2" | grep -E '\[dtr\] eval held-out' | tail -1 \
        | grep -oE 'acc [0-9.]+' | awk '{print $2}'
}

acc_ge()  { awk -v a="${1:-0}" -v b="$2" 'BEGIN { exit !(a+0 >= b+0) }'; }
acc_lt()  { awk -v a="${1:-0}" -v b="$2" 'BEGIN { exit !(a+0 <  b+0) }'; }

PASS=0; FAIL=0
ok()  { echo "  PASS  $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL  $1"; FAIL=$((FAIL + 1)); }

now_s() { date +%s; }

# victims: the K highest node ids (node 1 stays alive as the trainer/driver)
VICTIMS=$(seq $((N - K + 1)) "$N")
REVIVE=$N                       # the one we bring back in Phase 3
SURVIVORS=$(seq 1 $((N - K)))

echo "[bench] survival benchmark  N=$N  K=$K  victims={$(echo $VICTIMS | tr ' ' ',')}  revive=node$REVIVE  port=$PORT"

# ===========================================================================
# Boot: relay + N nodes
# ===========================================================================
echo "[bench] starting relay on :$PORT"
"$ROOT/relay/relay" -p "$PORT" -v >"${LOGPFX}_relay.log" 2>&1 & RELAY_PID=$!
sleep 1

echo "[bench] launching nodes 1..$N (FIFO-driven shells)"
for id in $(seq 1 "$N"); do
    start_node "$id"
    sleep 0.15                   # stagger the SWIM bring-up
done

echo "[bench] settling ${SETTLE}s (SWIM mesh + region formation)"
sleep "$SETTLE"

# ===========================================================================
# Phase 1 — learn on node 1, propagate to everyone, all eval trained
# ===========================================================================
echo
echo "================ PHASE 1: learn + propagate (N=$N) ================"
P1_T0=$(now_s)

# honest baseline: untrained accuracy on node 1 (recorded, not asserted)
M=$(mark 1); send 1 "dtr eval"
wait_log 1 "$M" '\[dtr\] eval held-out' 15
BASE_ACC=$(held_out_acc 1 "$M")
echo "[bench] node1 untrained baseline: held-out acc ${BASE_ACC:-?}%"

M=$(mark 1); send 1 "dtr train"
if wait_log 1 "$M" '\[dtr\] trained [0-9]+ epochs' 120; then
    ok "node1 training completed"
else
    bad "node1 training did not complete in 120s"
fi
wait_log 1 "$M" '\[dtr\] eval held-out' 15
declare -A ACC1
ACC1[1]=$(held_out_acc 1 "$M")
if acc_ge "${ACC1[1]}" "$ACC_MIN"; then
    ok "node1 trained: held-out acc ${ACC1[1]}% >= ${ACC_MIN}%"
else
    bad "node1 trained acc ${ACC1[1]:-?}% < ${ACC_MIN}%"
fi

M=$(mark 1); send 1 "dtr save"
if wait_log 1 "$M" "saved as p-fs object" 30; then
    ok "node1 saved weights to p-fs 'dtr/weights'"
else
    bad "node1 'dtr save' did not confirm in 30s"
fi

# every other node: pull the blob (dtr load retries actively send WANT)
# then eval — the propagation proof.
PROP_FAILED=0
for id in $(seq 2 "$N"); do
    LOADED=0
    for try in $(seq 1 20); do
        M=$(mark "$id"); send "$id" "dtr load"
        if wait_log "$id" "$M" "weights loaded from p-fs object" 4; then
            LOADED=1; break
        fi
        sleep 2
    done
    if [ "$LOADED" -ne 1 ]; then
        bad "node$id never received 'dtr/weights' via p-fs gossip"
        PROP_FAILED=1; ACC1[$id]=""
        continue
    fi
    M=$(mark "$id"); send "$id" "dtr eval"
    wait_log "$id" "$M" '\[dtr\] eval held-out' 15
    ACC1[$id]=$(held_out_acc "$id" "$M")
    if acc_ge "${ACC1[$id]}" "$ACC_MIN"; then
        ok "node$id loaded + trained: held-out acc ${ACC1[$id]}%"
    else
        bad "node$id acc ${ACC1[$id]:-?}% < ${ACC_MIN}% after load"
        PROP_FAILED=1
    fi
done
[ "$PROP_FAILED" -eq 0 ] && ok "weights propagated to all $N nodes (every node evals trained)"

P1_SECS=$(( $(now_s) - P1_T0 ))
P1_ACCS=$(for id in $(seq 1 "$N"); do printf '%s' "n$id=${ACC1[$id]:-?}% "; done)
echo "[bench] phase1 done in ${P1_SECS}s: $P1_ACCS"

# ===========================================================================
# Phase 2 — SIGKILL K nodes, survivors must stay alive AND smart
# ===========================================================================
echo
echo "================ PHASE 2: kill K=$K, survive (N-K=$((N - K))) ================"
P2_T0=$(now_s)

for v in $VICTIMS; do
    echo "[bench] SIGKILL node$v (pid ${NODE_PID[$v]})"
    kill -9 "${NODE_PID[$v]}" 2>/dev/null
    NODE_PID[$v]=""
done

# --- SWIM convergence: node1's `nodes` must show K DEAD, N-1-K ALIVE -------
WANT_ALIVE=$(( N - 1 - K ))     # peers of node 1, excluding SELF
CONVERGED=0
SWIM_DEADLINE=$(( $(now_s) + 120 ))
while [ "$(now_s)" -lt "$SWIM_DEADLINE" ]; do
    M=$(mark 1); send 1 "nodes"
    sleep 2
    S=$(slice 1 "$M")
    N_DEAD=$( printf '%s\n' "$S" | grep -cE '^ +[0-9]+ +DEAD')
    N_ALIVE=$(printf '%s\n' "$S" | grep -cE '^ +[0-9]+ +ALIVE')
    if [ "$N_DEAD" -eq "$K" ] && [ "$N_ALIVE" -eq "$WANT_ALIVE" ]; then
        CONVERGED=1; break
    fi
    sleep 2
done
SWIM_SECS=$(( $(now_s) - P2_T0 ))
if [ "$CONVERGED" -eq 1 ]; then
    ok "SWIM converged in ${SWIM_SECS}s: 'nodes' shows $N_ALIVE ALIVE + $K DEAD (+self)"
else
    bad "SWIM never converged: last view $N_ALIVE ALIVE / $N_DEAD DEAD (want $WANT_ALIVE/$K)"
fi

# --- world map agrees -------------------------------------------------------
M=$(mark 1); send 1 "world"
wait_log 1 "$M" '\[world\] known nodes:' 10
W=$(slice 1 "$M")
W_LIVE=$(printf '%s\n' "$W" | grep -E '^  node[0-9]+ ' | grep -cE ' (alive|self) ')
W_DEAD=$(printf '%s\n' "$W" | grep -E '^  node[0-9]+ ' | grep -cE ' DEAD ')
if [ "$W_LIVE" -eq $(( N - K )) ] && [ "$W_DEAD" -eq "$K" ]; then
    ok "world map: $W_LIVE live + $W_DEAD DEAD (matches N-K=$((N - K)), K=$K)"
else
    bad "world map: $W_LIVE live / $W_DEAD DEAD (want $((N - K))/$K)"
fi

# --- every survivor still evals trained + still holds the blob -------------
declare -A ACC2
P2_DATA_OK=0; P2_DATA_TOTAL=0
for id in $SURVIVORS; do
    M=$(mark "$id"); send "$id" "dtr eval"
    wait_log "$id" "$M" '\[dtr\] eval held-out' 15
    ACC2[$id]=$(held_out_acc "$id" "$M")
    if acc_ge "${ACC2[$id]}" "$ACC_MIN"; then
        ok "survivor node$id still trained: held-out acc ${ACC2[$id]}%"
    else
        bad "survivor node$id acc ${ACC2[$id]:-?}% < ${ACC_MIN}%"
    fi

    # data survival: the 2560 B weight blob (20 B header + 635 float32)
    # must still be listed in the local p-fs replica
    P2_DATA_TOTAL=$((P2_DATA_TOTAL + 1))
    M=$(mark "$id"); send "$id" "pfs ls"
    wait_log "$id" "$M" '\[pfs\] blocks' 10
    if slice "$id" "$M" | grep -qE 'len=2560'; then
        P2_DATA_OK=$((P2_DATA_OK + 1))
    else
        bad "node$id 'pfs ls' lost the 2560 B weight blob"
    fi
done
[ "$P2_DATA_OK" -eq "$P2_DATA_TOTAL" ] && \
    ok "weight blob intact on all $P2_DATA_OK/$P2_DATA_TOTAL survivors (pfs ls len=2560)"

# the object is not just listed but readable: re-load on one survivor
M=$(mark 1); send 1 "dtr load"
if wait_log 1 "$M" "weights loaded from p-fs object" 10; then
    ok "survivor node1 re-reads 'dtr/weights' from its replica"
else
    bad "survivor node1 cannot re-read 'dtr/weights'"
fi

# --- kdds pub/sub still flows ------------------------------------------------
M=$(mark 1); send 1 "rgnpub"
wait_log 1 "$M" 'global fanout' 10
FANOUT=$(slice 1 "$M" | grep -oE 'global fanout = [0-9]+' | grep -oE '[0-9]+$' | head -1)
FANOUT="${FANOUT:-0}"
if [ "$FANOUT" -ge 1 ]; then
    ok "kdds pub fanout = $FANOUT peers (>=1; survivors still reachable)"
else
    bad "kdds pub fanout = $FANOUT (no peers reachable)"
fi

M=$(mark 1); send 1 "infer 50 20 90 5"
if wait_log 1 "$M" '\[infer\] => class' 20 && \
   ! slice 1 "$M" | grep -q '\[infer\] no result'; then
    ok "distributed infer round-trip on survivors returned a class"
else
    bad "distributed infer failed on the surviving cluster"
fi

P2_SECS=$(( $(now_s) - P2_T0 ))
echo "[bench] phase2 done in ${P2_SECS}s (SWIM convergence ${SWIM_SECS}s)"

# ===========================================================================
# Phase 3 — one killed node returns and inherits the flock's memory
# ===========================================================================
echo
echo "================ PHASE 3: node$REVIVE returns ================"
P3_T0=$(now_s)

echo "[bench] restarting node$REVIVE (fresh process, untrained weights)"
start_node "$REVIVE"

# rejoin: a survivor must see node$REVIVE ALIVE again
REJOINED=0
REJOIN_DEADLINE=$(( $(now_s) + 120 ))
while [ "$(now_s)" -lt "$REJOIN_DEADLINE" ]; do
    M=$(mark 1); send 1 "nodes"
    sleep 2
    if slice 1 "$M" | grep -qE "^ +$REVIVE +ALIVE"; then
        REJOINED=1; break
    fi
    sleep 2
done
REJOIN_SECS=$(( $(now_s) - P3_T0 ))
if [ "$REJOINED" -eq 1 ]; then
    ok "node$REVIVE rejoined the mesh in ${REJOIN_SECS}s (survivor sees ALIVE)"
else
    bad "node$REVIVE never seen ALIVE again by node1"
fi

# honest before-number: the fresh individual is untrained
M=$(mark "$REVIVE"); send "$REVIVE" "dtr eval"
wait_log "$REVIVE" "$M" '\[dtr\] eval held-out' 20
ACC3_BEFORE=$(held_out_acc "$REVIVE" "$M")
if acc_lt "${ACC3_BEFORE:-0}" "$ACC_MIN"; then
    ok "revived node$REVIVE starts untrained: held-out acc ${ACC3_BEFORE:-?}%"
else
    bad "revived node$REVIVE already at ${ACC3_BEFORE:-?}% before load (?)"
fi

# inherit: re-fetch the weights through p-fs gossip
LOADED=0
for try in $(seq 1 30); do
    M=$(mark "$REVIVE"); send "$REVIVE" "dtr load"
    if wait_log "$REVIVE" "$M" "weights loaded from p-fs object" 4; then
        LOADED=1; break
    fi
    sleep 2
done
if [ "$LOADED" -eq 1 ]; then
    ok "node$REVIVE re-fetched 'dtr/weights' via p-fs gossip"
else
    bad "node$REVIVE never received 'dtr/weights' after rejoin"
fi

M=$(mark "$REVIVE"); send "$REVIVE" "dtr eval"
wait_log "$REVIVE" "$M" '\[dtr\] eval held-out' 15
ACC3=$(held_out_acc "$REVIVE" "$M")
if acc_ge "${ACC3:-0}" "$ACC_MIN"; then
    ok "node$REVIVE inherits the flock's memory: held-out acc ${ACC3}%"
else
    bad "node$REVIVE acc ${ACC3:-?}% < ${ACC_MIN}% after re-fetch"
fi

P3_SECS=$(( $(now_s) - P3_T0 ))
echo "[bench] phase3 done in ${P3_SECS}s"

# ===========================================================================
# Summary
# ===========================================================================
min_acc() {                      # min over the given assoc-array values
    local -n arr=$1; local m=""
    for k in "${!arr[@]}"; do
        v="${arr[$k]}"
        [ -z "$v" ] && { echo "?"; return; }
        if [ -z "$m" ] || acc_lt "$v" "$m"; then m="$v"; fi
    done
    echo "${m:-?}"
}

P1_MIN=$(min_acc ACC1)
P2_MIN=$(min_acc ACC2)

echo
echo "================ SURVIVAL BENCH RESULT (N=$N, K=$K) ================"
echo
echo "| phase | live nodes | held-out acc (min) | data survival | elapsed (s) |"
echo "|---|---|---|---|---|"
echo "| 1 learn + propagate | $N/$N | ${P1_MIN}% (all $N nodes) | dtr/weights on $N/$N nodes | $P1_SECS |"
echo "| 2 kill $K (SIGKILL) | $((N - K))/$N | ${P2_MIN}% (all survivors) | blob intact on $P2_DATA_OK/$P2_DATA_TOTAL survivors | $P2_SECS (SWIM $SWIM_SECS) |"
echo "| 3 node$REVIVE returns | $((N - K + 1))/$N | ${ACC3_BEFORE:-?}% -> ${ACC3:-?}% (revived) | re-fetched via p-fs gossip | $P3_SECS (rejoin $REJOIN_SECS) |"
echo
echo "  untrained baseline: ${BASE_ACC:-?}% held-out  |  trained threshold: >=${ACC_MIN}%"
echo "  kdds pub fanout after kill: $FANOUT peers"
echo "  logs: ${LOGPFX}_node{1..$N}.log  ${LOGPFX}_relay.log"
echo
echo "================ SUMMARY ================"
echo "  N=$N K=$K   PASS=$PASS  FAIL=$FAIL"
if [ "$FAIL" -eq 0 ]; then
    echo "  RESULT: PASS  (killed $K of $N; cluster stayed alive, smart, and re-teachable)"
    exit 0
else
    echo "  RESULT: FAIL  ($FAIL assertion(s) failed -- see above)"
    exit 1
fi
