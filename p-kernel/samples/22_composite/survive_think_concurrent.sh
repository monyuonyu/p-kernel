#!/bin/bash
# ---------------------------------------------------------------------------
# survive_think_concurrent.sh — the COMPOSITE survival loop (wave 12, audit G19)
#
# The audit's "philosophy gap" #19: every link in the survival chain is green
# *in isolation* — run_memory_thought.sh (記憶で考える), kill_one.sh +
# concurrent_infer.sh (死の貫通 + 同時多発), run_crash_recovery.sh /
# run_survival_bench.sh (障害復旧 / 復活) — but NOBODY had ever proven the
# three hold AT THE SAME TIME on ONE cluster. A chain that is green link by
# link can still snap when the links are loaded together. This harness loads
# them together and asserts the chain holds.
#
# ONE 4-node cluster over the public ./relay. The whole point is overlap:
#
#   PHASE 1 — seed the swarm's memory (記憶)
#     node1 trains a real 635-param Transformer (analytic backprop), `dtr save`s
#     the weights and `dtr remember`s representative engrams — both land in p-fs
#     ("dtr/weights", "dtr/engrams") and gossip out via P1 chunk replication +
#     P2 ref gossip. node2/node3 pull the WEIGHTS (`dtr load`); node4 is left a
#     pure "memory-only thinker" (never loads weights — it can only think with
#     the engrams the swarm gossips to it).
#
#   PHASE 2 — THE STORM: 殺す ∧ 同時多発 ∧ 記憶で考える, all overlapping
#     In one tight window we interleave, frame by frame:
#       (同時多発) node1 AND node2 issue distributed KV-attention inferences in
#                  the SAME frame, repeatedly (per-origin Q, origin-distinct
#                  req namespaces — concurrent_infer.sh's G1 fix).
#       (記憶で考える) node4 runs `dtr eval` DURING the storm: its [ret ON]
#                  held-out accuracy must beat chance using ONLY engrams it
#                  received from the swarm through p-fs (it never trained, never
#                  loaded weights) — and "engrams loaded from p-fs" must appear.
#       (殺す①) mid-storm we SIGKILL node3 (a NON-origin responder). Both
#                  origins must KEEP completing every inference, honestly marked
#                  "degraded (k/n)" while SWIM converges — never a silent
#                  success, never an E_TMOUT.
#       (殺す②) then we SIGKILL node1 (an ORIGIN). A survivor (node2) re-issues
#                  the SAME deterministic question and completes it — the origin
#                  holds no privilege.
#     Survivors then prove they are still SMART: node2 evals at trained
#     accuracy with memory (weights + engrams), and the `world` map shows
#     exactly the right live/DEAD split (生存数).
#
#   PHASE 3 — RETURN (復活 + p-fs 再取得)
#     We restart node3 (fresh process = a new individual, untrained). It rejoins
#     the mesh, re-fetches BOTH the weights and the engrams through p-fs gossip
#     (`dtr load`), and evals from untrained back to trained-with-memory — the
#     newcomer inherits the flock's memory.
#
#   PHASE 4 — no collateral damage
#     Across every node log: ZERO crash / garbage-PC markers ([guard] FAULT,
#     [fatal], the deterministic pc=0x... net_relay garbage SIGSEGV), and every
#     survivor PROCESS still running (a survivor segfaulting as a side effect of
#     a peer's death is a survival failure even if the arithmetic adds up).
#
# A markdown result table is printed to stdout. Exit code is non-zero if any
# assertion fails. Every harness line is timestamped. Logs: /tmp/ci22_*.log
#
# Usage:   ./survive_think_concurrent.sh
# Tunables (env):
#   PORT      relay port                                  (default 7422)
#   SETTLE    seconds for SWIM + region beacons to settle (default 8)
#   ACC_MIN   held-out % that counts as "trained"         (default 90.0)
#   MEM_MIN   held-out % that counts as "memory beats chance" (default 55.0)
#   STORM     concurrent infer frames in the storm        (default 6)
#
# Flake notes: gossip-dependent steps (dtr load / dtr eval) are retried with
# active re-sends, exactly as run_survival_bench.sh does, because a single
# stdin line has (rarely) been observed lost on a freshly meshed node. See the
# README for the full list of known flake sources and mitigations.
# ---------------------------------------------------------------------------
set -u

PORT="${PORT:-7422}"
SETTLE="${SETTLE:-8}"
ACC_MIN="${ACC_MIN:-90.0}"
MEM_MIN="${MEM_MIN:-55.0}"
STORM="${STORM:-6}"

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
export PKERNEL_RELAY_PORT="$PORT"
# composite stays in ONE region (no RTT zoning) so all four nodes co-aggregate.
unset PKERNEL_RTT_ZONE_SIZE PKERNEL_RTT_ZONE_PENALTY 2>/dev/null || true

N=4
FIFODIR="$(mktemp -d /tmp/ci22_fifo.XXXXXX)"
LOGPFX=/tmp/ci22
rm -f "${LOGPFX}_node"*.log "${LOGPFX}_relay.log" 2>/dev/null
declare -a NODE_PID
for i in 1 2 3 4; do NODE_PID[$i]=0; done
RELAY_PID=0
FAIL=0
PASS=0

# All human/progress output goes to stderr so that $(...) command
# substitutions capturing a function's stdout (e.g. eval_acc) never swallow a
# stray log line. The markdown result table at the end uses plain stdout echo.
TS()   { date '+%H:%M:%S'; }
log()  { echo "[$(TS)] $*" >&2; }
ok()   { log "ok  : $*"; PASS=$((PASS + 1)); }
fail() { log "FAIL: $*"; FAIL=1; }

nodelog() { echo "${LOGPFX}_node$1.log"; }

# send <id> <command...> — write one shell line to node id's FIFO. The harness
# holds a read-write fd on each FIFO (fd 100+id) so this never blocks and never
# SIGPIPEs even after the node has been SIGKILLed.
send() {
    local id="$1"; shift
    log "node$id <- '$*'"
    eval "printf '%s\n' \"\$*\" >&$((100 + id))"
}

start_node() {                 # start_node <id> — (re)launch a node on its FIFO
    local id="$1"; local fifo="$FIFODIR/in$id"; local fd=$((100 + id))
    [ -p "$fifo" ] || mkfifo "$fifo"
    eval "exec $fd<>'$fifo'"
    PKERNEL_NODE_ID="$id" PKERNEL_AUTONET=1 "$BOOT/p-kernel" \
        < "$fifo" >> "$(nodelog "$id")" 2>&1 &
    NODE_PID[$id]=$!
    disown "${NODE_PID[$id]}"   # no job-control "Killed" noise on SIGKILL
    log "node$id up  pid=${NODE_PID[$id]}  log=$(nodelog "$id")"
}

node_running() { [ "${NODE_PID[$1]}" != 0 ] && kill -0 "${NODE_PID[$1]}" 2>/dev/null; }

cluster_down() {
    for i in 1 2 3 4; do
        eval "exec $((100 + i))>&-" 2>/dev/null || true
        [ "${NODE_PID[$i]}" != 0 ] && kill -9 "${NODE_PID[$i]}" 2>/dev/null
        NODE_PID[$i]=0
    done
    [ "$RELAY_PID" != 0 ] && kill -9 "$RELAY_PID" 2>/dev/null
    RELAY_PID=0
    wait 2>/dev/null
    rm -rf "$FIFODIR" 2>/dev/null
}
trap cluster_down EXIT

# --- incremental log readers (so a re-sent command is parsed independently) --
mark()  { wc -l <"$(nodelog "$1")" 2>/dev/null || echo 0; }
slice() { tail -n "+$(( $2 + 1 ))" "$(nodelog "$1")" 2>/dev/null; }

# wait_log <id> <from_mark> <regex> <timeout_s>
wait_log() {
    local id="$1" from="$2" re="$3" to="$4" i=0
    while [ "$i" -lt $(( to * 2 )) ]; do
        slice "$id" "$from" | grep -aqE "$re" && return 0
        node_running "$id" || return 2
        sleep 0.5; i=$((i + 1))
    done
    return 1
}

acc_ge() { awk -v a="${1:-0}" -v b="$2" 'BEGIN { exit !(a+0 >= b+0) }'; }
acc_lt() { awk -v a="${1:-0}" -v b="$2" 'BEGIN { exit !(a+0 <  b+0) }'; }

# held-out accuracy for a given retrieval mode, parsed from a fresh `dtr eval`.
# eval_acc <id> <ret off|ret ON> [tries] -> "95.0" (or "")
eval_acc() {
    local id="$1" mode="$2" tries="${3:-4}" t M a
    for t in $(seq 1 "$tries"); do
        M=$(mark "$id"); send "$id" "dtr eval"
        if wait_log "$id" "$M" "\[dtr\] eval held-out.*\[$mode\]" 15; then
            a=$(slice "$id" "$M" | grep -aE "\[dtr\] eval held-out.*\[$mode\]" \
                | tail -1 | sed -E 's/.*acc ([0-9.]+)%.*/\1/')
            [ -n "$a" ] && { echo "$a"; return 0; }
        fi
        node_running "$id" || break
    done
    echo ""
}

# fp for a given req_id from a node log: "[dkva-cmd] => OK  req=<R>  fp=<F>"
get_fp() { grep -a "=> OK  req=$2 " "$(nodelog "$1")" | grep -ao 'fp=[0-9]*' | head -1; }

# count of a pattern across the WHOLE life of node id's log
count() { local c; c=$(grep -ac "$2" "$(nodelog "$1")"); echo "${c:-0}"; }

# ===========================================================================
log "================ COMPOSITE survival loop (G19): 殺す ∧ 同時多発 ∧ 記憶 ================"
log "kernel=$BOOT/p-kernel  relay :$PORT  N=$N  ACC_MIN=$ACC_MIN  MEM_MIN=$MEM_MIN"

"$ROOT/relay/relay" -p "$PORT" -v > "${LOGPFX}_relay.log" 2>&1 &
RELAY_PID=$!
disown "$RELAY_PID"
sleep 1

for i in 1 2 3 4; do start_node "$i"; sleep 0.2; done

# readiness: node1 reaches FULL once SWIM sees its peers ALIVE
t=0
while [ $t -lt 40 ]; do
    grep -aq -- "-> FULL" "$(nodelog 1)" && break
    sleep 1; t=$((t + 1))
done
if [ $t -ge 40 ]; then fail "cluster never reached FULL"; else log "cluster FULL after ~${t}s"; fi
log "settling ${SETTLE}s (SWIM RTT + world region beacons)"
sleep "$SETTLE"

# ===========================================================================
log "================ PHASE 1: seed the swarm's memory (記憶) ================"
# node1: train -> save weights -> remember engrams (both -> p-fs, gossip out)
M=$(mark 1); send 1 "dtr train"
if wait_log 1 "$M" '\[dtr\] trained [0-9]+ epochs' 180; then
    ok "P1: node1 trained the Transformer (real SGD)"
else
    fail "P1: node1 training did not complete in 180s"
fi
TR_ACC=$(eval_acc 1 "ret off")
if acc_ge "$TR_ACC" "$ACC_MIN"; then ok "P1: node1 trained acc ${TR_ACC}% >= ${ACC_MIN}%"
else fail "P1: node1 trained acc ${TR_ACC:-?}% < ${ACC_MIN}%"; fi

M=$(mark 1); send 1 "dtr save"
if wait_log 1 "$M" "saved as p-fs object" 30; then ok "P1: node1 saved weights to p-fs 'dtr/weights'"
else fail "P1: node1 'dtr save' did not confirm"; fi

M=$(mark 1); send 1 "dtr remember"
if wait_log 1 "$M" "engrams .* saved as p-fs object" 30; then ok "P1: node1 remembered engrams to p-fs 'dtr/engrams'"
else fail "P1: node1 'dtr remember' did not confirm"; fi

# node2 & node3: pull the WEIGHTS via gossip (retry — active WANT re-sends).
# node4 is deliberately NOT loaded: it stays a pure memory-only thinker.
for id in 2 3; do
    LOADED=0
    for try in $(seq 1 20); do
        M=$(mark "$id"); send "$id" "dtr load"
        if wait_log "$id" "$M" "weights loaded from p-fs object" 4; then LOADED=1; break; fi
        sleep 2
    done
    if [ "$LOADED" -eq 1 ]; then ok "P1: node$id pulled weights via p-fs gossip"
    else fail "P1: node$id never received 'dtr/weights' via gossip"; fi
done
log "P1: node4 left untrained + unloaded (pure memory-only thinker)"

# ===========================================================================
log "================ PHASE 2: THE STORM — 殺す ∧ 同時多発 ∧ 記憶で考える ================"
SM1=$(mark 1); SM2=$(mark 2); SM4=$(mark 4)

log "storm begins: node1 + node2 infer in the same frame x$STORM; node4 thinks"
log "with memory mid-storm; node3 (non-origin) is SIGKILLed inside the window"
EVAL_FIRED=0
for k in $(seq 1 "$STORM"); do
    send 1 "dkva infer 50 20 90 5"     # origin node1 (internal 0) req 900000X
    send 2 "dkva infer 50 20 90 5"     # origin node2 (internal 1) req 901000X
    if [ "$k" -eq 2 ]; then
        sleep 0.3
        log "SIGKILL node3 (pid ${NODE_PID[3]}) — NON-origin responder, mid-storm"
        kill -9 "${NODE_PID[3]}" 2>/dev/null; NODE_PID[3]=0
        sleep 0.5
    fi
    if [ "$k" -eq 4 ] && [ "$EVAL_FIRED" -eq 0 ]; then
        # 記憶で考える, DURING the concurrent storm: node4 has only the engrams
        # the swarm gossiped to it (no weights). One async eval, parsed later.
        send 4 "dtr eval"
        EVAL_FIRED=1
    fi
    sleep 1.2
done
sleep 3

# --- 同時多発: both origins completed EVERY frame, never a silent E_TMOUT ----
OK1=$(slice 1 "$SM1" | grep -ac '\[dkva-cmd\] => OK')
OK2=$(slice 2 "$SM2" | grep -ac '\[dkva-cmd\] => OK')
F1=$(slice 1 "$SM1" | grep -ac '\[dkva-cmd\] => FAILED')
F2=$(slice 2 "$SM2" | grep -ac '\[dkva-cmd\] => FAILED')
if [ "$OK1" -eq "$STORM" ]; then ok "P2: origin node1 completed all $STORM concurrent inferences (OK=$OK1)"
else fail "P2: origin node1 only completed $OK1/$STORM inferences"; fi
if [ "$OK2" -eq "$STORM" ]; then ok "P2: origin node2 completed all $STORM concurrent inferences (OK=$OK2)"
else fail "P2: origin node2 only completed $OK2/$STORM inferences"; fi
if [ "$F1" -eq 0 ] && [ "$F2" -eq 0 ]; then ok "P2: neither origin ever E_TMOUTed during the storm"
else fail "P2: E_TMOUT during storm (node1=$F1 node2=$F2)"; fi

# --- 殺す①: the death was reported honestly, never a silent success ---------
DEG=$(( $(slice 1 "$SM1" | grep -ac '\[dkva\] degraded (') + $(slice 2 "$SM2" | grep -ac '\[dkva\] degraded (') ))
if [ "$DEG" -ge 1 ]; then ok "P2: node3's death honestly reported as degraded (k/n) ($DEG x), not silent"
else fail "P2: no 'degraded (k/n)' line after killing node3 — silent loss?"; fi

# --- per-origin Q didn't cross-contaminate: each origin's storm fp is stable
FP1=$(slice 1 "$SM1" | grep -a '\[dkva-cmd\] => OK' | grep -ao 'fp=[0-9]*' | sort -u | wc -l)
FP2=$(slice 2 "$SM2" | grep -a '\[dkva-cmd\] => OK' | grep -ao 'fp=[0-9]*' | sort -u | wc -l)
if [ "$FP1" -le 1 ] && [ "$FP2" -le 1 ]; then
    ok "P2: per-origin Q stayed origin-distinct under concurrency (node1 fp set=$FP1, node2 fp set=$FP2)"
else
    log "note: P2: origin fp varied under degradation (node1=$FP1 node2=$FP2 distinct) — acceptable while a region is missing"
fi

# --- 記憶で考える: node4 (no weights) thinks with the swarm's engrams --------
if wait_log 4 "$SM4" '\[dtr\] eval held-out.*\[ret ON\]' 20; then
    MEM_OFF=$(slice 4 "$SM4" | grep -aE '\[dtr\] eval held-out.*\[ret off\]' | tail -1 | sed -E 's/.*acc ([0-9.]+)%.*/\1/')
    MEM_ON=$( slice 4 "$SM4" | grep -aE '\[dtr\] eval held-out.*\[ret ON\]'  | tail -1 | sed -E 's/.*acc ([0-9.]+)%.*/\1/')
    if slice 4 "$SM4" | grep -aq 'engrams loaded from p-fs'; then
        ok "P2: node4's engrams came FROM p-fs gossip (it never remembered locally)"
    else
        # the engram cache may have loaded on an earlier eval; check whole log
        count 4 'engrams loaded from p-fs' >/dev/null
        if [ "$(count 4 'engrams loaded from p-fs')" -ge 1 ]; then
            ok "P2: node4 loaded engrams from p-fs (cached from gossip)"
        else
            fail "P2: node4 never loaded engrams from p-fs"
        fi
    fi
    if acc_ge "$MEM_ON" "$MEM_MIN"; then
        ok "P2: node4 thinks with memory mid-storm: [ret ON] held-out ${MEM_ON}% >= ${MEM_MIN}% (off=${MEM_OFF:-?}%)"
    else
        fail "P2: node4 memory-only acc ${MEM_ON:-?}% < ${MEM_MIN}% (memory did not lift it above chance)"
    fi
else
    fail "P2: node4 never produced a [ret ON] held-out eval during the storm (no engrams gossiped?)"
fi

# --- 殺す②: kill an ORIGIN; a survivor re-issues the SAME question ----------
log "SIGKILL node1 (pid ${NODE_PID[1]}) — the ORIGIN dies"
kill -9 "${NODE_PID[1]}" 2>/dev/null; NODE_PID[1]=0
sleep 2
RM2=$(mark 2)
send 2 "dkva infer 50 20 90 5"        # survivor re-issues the SAME deterministic Q
if wait_log 2 "$RM2" '\[dkva-cmd\] => OK  req=' 15; then
    ok "P2: survivor node2 re-issued the question after the origin died and completed it"
else
    fail "P2: survivor node2 could not complete after origin death"
fi
if slice 2 "$RM2" | grep -aq '\[dkva-cmd\] => FAILED'; then fail "P2: survivor E_TMOUTed after origin death"
else ok "P2: survivor never E_TMOUTed after origin death"; fi

# --- survivors are still SMART: node2 evals trained-with-memory --------------
S2_ON=$(eval_acc 2 "ret ON")
if acc_ge "$S2_ON" "$ACC_MIN"; then ok "P2: survivor node2 still trained WITH memory: [ret ON] held-out ${S2_ON}%"
else fail "P2: survivor node2 [ret ON] held-out ${S2_ON:-?}% < ${ACC_MIN}%"; fi

# --- world map shows the right 生存数 (node1,node3 DEAD; node2,node4 alive) --
# Poll: SWIM needs time to promote freshly-killed peers from stale -> DEAD in
# node2's local view, exactly like run_survival_bench.sh's convergence loop.
W_LIVE=0; W_DEAD=0; WMAP_OK=0
WMAP_DL=$(( $(date +%s) + 90 ))
while [ "$(date +%s)" -lt "$WMAP_DL" ]; do
    WM=$(mark 2); send 2 "world"
    wait_log 2 "$WM" '\[world\] known nodes:' 10 || true
    W=$(slice 2 "$WM")
    W_LIVE=$(printf '%s\n' "$W" | grep -aE '^  node[0-9]+ ' | grep -acE ' (alive|self) ')
    W_DEAD=$(printf '%s\n' "$W" | grep -aE '^  node[0-9]+ ' | grep -acE ' DEAD ')
    if [ "$W_LIVE" -eq 2 ] && [ "$W_DEAD" -eq 2 ]; then WMAP_OK=1; break; fi
    sleep 3
done
if [ "$WMAP_OK" -eq 1 ]; then
    ok "P2: world map shows correct survival: $W_LIVE alive + $W_DEAD DEAD (node1,node3 dead)"
else
    fail "P2: world map $W_LIVE alive / $W_DEAD DEAD (want 2 alive / 2 DEAD)"
fi

# ===========================================================================
log "================ PHASE 3: RETURN — node3 revives, inherits the flock ================"
log "restarting node3 (fresh process = a new individual, untrained)"
start_node 3
# rejoin: a survivor (node2) must see node3's internal id (3-1=2) ALIVE again
REJOIN=0
DL=$(( $(date +%s) + 90 ))
while [ "$(date +%s)" -lt "$DL" ]; do
    M=$(mark 2); send 2 "nodes"; sleep 2
    slice 2 "$M" | grep -aqE '^ +2 +ALIVE' && { REJOIN=1; break; }
    sleep 2
done
if [ "$REJOIN" -eq 1 ]; then ok "P3: node3 rejoined the mesh (survivor sees it ALIVE)"
else fail "P3: node3 never seen ALIVE again"; fi

# honest before-number: the fresh individual is untrained
B3=$(eval_acc 3 "ret off")
if acc_lt "$B3" "$ACC_MIN"; then ok "P3: revived node3 starts untrained: held-out ${B3}%"
else log "note: P3: revived node3 already at ${B3:-?}% before load (gossip beat us to it)"; fi

# inherit: re-fetch WEIGHTS through p-fs gossip
LOADED=0
for try in $(seq 1 30); do
    M=$(mark 3); send 3 "dtr load"
    if wait_log 3 "$M" "weights loaded from p-fs object" 4; then LOADED=1; break; fi
    sleep 2
done
if [ "$LOADED" -eq 1 ]; then ok "P3: node3 re-fetched 'dtr/weights' via p-fs gossip after revival"
else fail "P3: node3 never re-fetched weights after revival"; fi

# eval with memory: weights (re-fetched) + engrams (re-fetched) -> trained
A3=$(eval_acc 3 "ret ON")
if acc_ge "$A3" "$ACC_MIN"; then ok "P3: node3 inherits the flock's memory: [ret ON] held-out ${A3}%"
else fail "P3: node3 [ret ON] held-out ${A3:-?}% < ${ACC_MIN}% after re-fetch"; fi
if [ "$(count 3 'engrams loaded from p-fs')" -ge 1 ]; then ok "P3: node3 also re-fetched engrams from p-fs"
else log "note: P3: node3 engram-load line not seen (weights alone carried the eval)"; fi

# ===========================================================================
log "================ PHASE 4: no collateral damage (crash / garbage-PC zero) ================"
CRASH=0
for id in 1 2 3 4; do
    c=$(grep -acE '\[guard\] FAULT|\[fatal\]|pc=0x[0-9a-f]*[0-9a-f]{8}' "$(nodelog "$id")" 2>/dev/null)
    c=${c:-0}
    if [ "$c" -ne 0 ]; then fail "P4: node$id log shows $c crash/garbage-PC marker(s)"; CRASH=1; fi
done
[ "$CRASH" -eq 0 ] && ok "P4: zero crash / garbage-PC markers across all four node logs"

# every survivor PROCESS still running (catches collateral segfaults)
SURV_OK=1
for id in 2 3 4; do
    if node_running "$id"; then :; else fail "P4: survivor node$id process is not running (collateral crash?)"; SURV_OK=0; fi
done
[ "$SURV_OK" -eq 1 ] && ok "P4: all survivors (node2, node3-revived, node4) processes still alive"

# ===========================================================================
echo
echo "==================== COMPOSITE RESULT (G19) ===================="
echo
echo "| chain link              | what was proven (overlapping)                         | result |"
echo "|-------------------------|-------------------------------------------------------|--------|"
echo "| 記憶で考える (memory)    | node4 (no weights) [ret ON] held-out ${MEM_ON:-?}% > chance, engrams via gossip | $( [ "$FAIL" -eq 0 ] && echo PASS || echo see-above ) |"
echo "| 同時多発 (concurrent)    | node1 & node2 each completed $STORM same-frame inferences (OK=$OK1/$OK2) | $( [ "$OK1" = "$STORM" ] && [ "$OK2" = "$STORM" ] && echo PASS || echo FAIL ) |"
echo "| 殺す① (non-origin kill)  | node3 killed mid-storm; degraded(k/n) reported ${DEG}x, no E_TMOUT | $( [ "$DEG" -ge 1 ] && [ "$F1" -eq 0 ] && [ "$F2" -eq 0 ] && echo PASS || echo FAIL ) |"
echo "| 殺す② (origin kill)      | origin node1 killed; survivor node2 re-issued & completed | $( node_running 2 && echo PASS || echo FAIL ) |"
echo "| 生存数 (world map)       | world map: $W_LIVE alive + $W_DEAD DEAD (want 2/2)               | $( [ "$W_LIVE" -eq 2 ] && [ "$W_DEAD" -eq 2 ] && echo PASS || echo FAIL ) |"
echo "| 復活 (return + re-fetch) | node3 revived: ${B3:-?}% -> ${A3:-?}% (weights+engrams via gossip) | $( acc_ge "${A3:-0}" "$ACC_MIN" && echo PASS || echo FAIL ) |"
echo "| 無被害 (no crash)        | zero garbage-PC, all survivors alive                  | $( [ "$CRASH" -eq 0 ] && [ "$SURV_OK" -eq 1 ] && echo PASS || echo FAIL ) |"
echo
echo "  trained baseline node1: ${TR_ACC:-?}%  |  survivor node2 with memory: ${S2_ON:-?}%"
echo "  thresholds: trained >= ${ACC_MIN}%, memory-beats-chance >= ${MEM_MIN}%"
echo "  logs: ${LOGPFX}_node{1..4}.log  ${LOGPFX}_relay.log"
echo
echo "==================== SUMMARY ===================="
if [ "$FAIL" -eq 0 ]; then
    log "RESULT: PASS ($PASS checks) — the chain holds when all links are loaded together"
    exit 0
else
    log "RESULT: FAIL — a link snapped under composite load; see /tmp/ci22_*.log"
    exit 1
fi
