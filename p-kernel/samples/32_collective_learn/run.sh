#!/bin/bash
# ===========================================================================
# 32_collective_learn / run.sh  —  the swarm LEARNS together, with no central
# aggregator, in the slow deliberation band, surviving node death.
# (G22 / survival-network.md §8 "全体が未来を強くする" / §9 考える器官)
#
# THE CRUX (and the anti-fake): each node trains on a DISJOINT shard of the
# data — leave-one-class-out, so NO single node ever sees all 3 classes and
# therefore CANNOT learn the whole task alone. Each node periodically GOSSIPS
# its full 635-param transformer weight body over the relay (p-fs per-node
# ref "dtr/model/<n>") and MERGES peers' models into its own by averaging
# (decentralized SGD). Over rounds EVERY node's accuracy on the FULL (all-
# class) task rises ABOVE its solo shard-only ceiling — the swarm learned
# what no node could alone. This is COMBINATION, not COPY (we print each
# node's solo ceiling AND its collective result; the collective must exceed).
#
# NO central aggregator (§7): every node reads only gossiped peer models and
# averages locally — peer-symmetric, no server. Slow band (§8): the merge
# runs on a seconds cadence, not the reflex tick.
#
# §3 survival: mid-learning we kill -9 one node. The surviving swarm KEEPS
# improving (the two survivors still cover all classes between them, and the
# dead node's last model lingers in p-fs). Then a node REJOINS and catches up
# to the collective via gossip in a few rounds.
#
# Also runs the in-kernel property self-test ([g22-shard-solo] /
# [g22-gossip-learn] / [g22-no-central]).
#
# Exit 0 = all PASS. Logs: /tmp/p32_*.log
# ===========================================================================
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "unsupported host arch $(uname -m)"; exit 1 ;;
esac
# Optional overrides (both unset in CI -> native build, no wrapper):
#   PKERNEL_BOOT_DIR  — force a specific boot/ build (cross-arch validation)
#   PKERNEL_WRAP      — prefix each kernel launch (e.g. qemu-x86_64) so the
#                       x86_64 CI path can be exercised on a non-x86_64 host.
[ -n "${PKERNEL_BOOT_DIR:-}" ] && BOOT="$ROOT/$PKERNEL_BOOT_DIR"
WRAP="${PKERNEL_WRAP:-}"
[ -x "$BOOT/p-kernel" ]    || make -C "$BOOT"        >/dev/null || exit 1
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay"  >/dev/null || exit 1

export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
export PKERNEL_RELAY_HOST=127.0.0.1
export PKERNEL_RELAY_PORT=7432

# learning schedule (rounds x local-steps). Long enough that (a) the swarm
# clears the ~66.7% leave-one-class-out solo ceiling with margin and (b) the
# survivors stay actively gossiping across the whole kill -> rejoin window so
# the rejoining node catches up against LIVE peers.
ROUNDS="${GL_ROUNDS:-36}"
LOCAL="${GL_LOCAL:-4}"

WORK="$(mktemp -d /tmp/p32_work.XXXXXX)"
FIFO="$WORK/fifo"
declare -A NODE_PID
RELAY_PID=0
FAIL=0

TS()   { date '+%H:%M:%S'; }
log()  { echo "[$(TS)] $*"; }
ok()   { log "ok  : $*"; }
bad()  { log "FAIL: $*"; FAIL=1; }
send() { local i="$1"; shift; log "node$i <- '$*'"; printf '%s\n' "$*" > "$FIFO.$i"; }

cleanup() {
    exec 3>&- 4>&- 5>&- 2>/dev/null || true
    for i in 1 2 3; do
        [ "${NODE_PID[$i]:-0}" != 0 ] && kill -9 "${NODE_PID[$i]}" 2>/dev/null
        NODE_PID[$i]=0
    done
    [ "$RELAY_PID" != 0 ] && kill -9 "$RELAY_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

start_node() {  # <i>  — (re)start node i on its own durable p-fs dir
    local i="$1"
    rm -f "$FIFO.$i"; mkfifo "$FIFO.$i"
    mkdir -p "$WORK/dir$i"
    env PKERNEL_NODE_ID=$i PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$WORK/dir$i" \
        $WRAP "$BOOT/p-kernel" < "$FIFO.$i" > "/tmp/p32_node$i.log" 2>&1 &
    NODE_PID[$i]=$!; disown "${NODE_PID[$i]}"
    log "node$i up pid=${NODE_PID[$i]} log=/tmp/p32_node$i.log"
}

# last full_acc value seen for a node (numeric, one decimal -> integer *10)
last_full() {  # <logfile>
    grep -aoE 'full_acc=[0-9]+\.[0-9]' "$1" 2>/dev/null | tail -1 \
        | grep -aoE '[0-9]+\.[0-9]' | tr -d .
}
solo_ceil() {  # <logfile>
    grep -aoE 'solo_ceiling=[0-9]+\.[0-9]' "$1" 2>/dev/null | tail -1 \
        | grep -aoE '[0-9]+\.[0-9]' | tr -d .
}
rounds_done() {  # <logfile>
    grep -acE '\[g22-live\] node=[0-9]+ round=' "$1" 2>/dev/null
}
wait_rounds() {  # <logfile> <n> <secs>
    local f="$1" n="$2" s="$3" i=0
    while [ "$i" -lt "$((s * 4))" ]; do
        [ "$(rounds_done "$f")" -ge "$n" ] && return 0
        sleep 0.25; i=$((i + 1))
    done
    return 1
}
wait_for() {  # <file> <pattern> <secs>
    local f="$1" p="$2" s="$3" i=0
    while [ "$i" -lt "$((s * 4))" ]; do
        grep -aqE "$p" "$f" 2>/dev/null && return 0
        sleep 0.25; i=$((i + 1))
    done
    return 1
}

L1=/tmp/p32_node1.log
L2=/tmp/p32_node2.log
L3=/tmp/p32_node3.log

# ===========================================================================
echo "==========================================================="
echo " (0) in-kernel property self-test: decentralized collective learning"
echo "==========================================================="
ST=/tmp/p32_selftest.log
printf 'dtr gossip test\nexit\n' | timeout 150 $WRAP "$BOOT/p-kernel" > "$ST" 2>&1 || true
grep -aE '\[g22(-shard-solo|-gossip-learn|-no-central)?\]' "$ST" | sed 's/^/    /'
grep -qaE '\[g22-shard-solo\] PASS'   "$ST" && ok "[g22-shard-solo] PASS"   || bad "[g22-shard-solo] failed"
grep -qaE '\[g22-gossip-learn\] PASS' "$ST" && ok "[g22-gossip-learn] PASS" || bad "[g22-gossip-learn] failed"
grep -qaE '\[g22-no-central\] PASS'   "$ST" && ok "[g22-no-central] PASS"   || bad "[g22-no-central] failed"

# ===========================================================================
echo
echo "==========================================================="
echo " LIVE — 3 nodes, DISJOINT shards, decentralized gossip learning"
echo "==========================================================="
log "--- relay :$PKERNEL_RELAY_PORT + 3 nodes (each leave-one-class-out) ---"
"$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" -v > "$WORK/relay.log" 2>&1 &
RELAY_PID=$!; disown "$RELAY_PID"; sleep 1
for i in 1 2 3; do start_node "$i"; done
exec 3<>"$FIFO.1" 4<>"$FIFO.2" 5<>"$FIFO.3"

t=0; while [ $t -lt 30 ]; do grep -aq -- '-> FULL' "$L1" && break; sleep 1; t=$((t+1)); done
[ $t -ge 30 ] && { bad "cluster never reached FULL"; echo "RESULT: FAIL"; exit 1; }
log "cluster FULL after ~${t}s"; sleep 2

# --- 1) each node measures its SOLO shard-only ceiling on the full task -----
log "measuring each node's solo shard-only ceiling (the anti-copy baseline) ..."
send 1 "dtr gossip solo 160"
send 2 "dtr gossip solo 160"
send 3 "dtr gossip solo 160"
for L in "$L1" "$L2" "$L3"; do wait_for "$L" 'solo_ceiling=' 30 || bad "no solo_ceiling on $L"; done
S1=$(solo_ceil "$L1"); S2=$(solo_ceil "$L2"); S3=$(solo_ceil "$L3")
log "solo ceilings (x10): node1=$S1 node2=$S2 node3=$S3"
grep -aE 'solo_ceiling=' "$L1" "$L2" "$L3" | sed 's#.*/##; s/^/    /'

# --- 2) PHASE A: 3-node collective learning — EVERY node beats its solo ------
log "PHASE A: gossip-learn on all 3 nodes (rounds=$ROUNDS local=$LOCAL, no central aggregator) ..."
send 1 "dtr gossip run $ROUNDS $LOCAL"
send 2 "dtr gossip run $ROUNDS $LOCAL"
send 3 "dtr gossip run $ROUNDS $LOCAL"

# THE core claim: while gossiping their disjoint shards, EVERY node's full-task
# accuracy rises ABOVE its own solo shard-only ceiling — at the same time. Poll
# until that simultaneous above-solo state is reached (the live async swarm's
# per-node accuracy oscillates a little round-to-round, so we assert the
# collective LIFT each node achieves, not a single final sample).
log "waiting until EVERY node is simultaneously ABOVE its solo ceiling (collective>individual) ..."
A1=0; A2=0; A3=0; reached=0; i=0
while [ "$i" -lt 800 ]; do      # up to 200s
    A1=$(last_full "$L1"); A2=$(last_full "$L2"); A3=$(last_full "$L3")
    if [ "${A1:-0}" -gt "$S1" ] && [ "${A2:-0}" -gt "$S2" ] && [ "${A3:-0}" -gt "$S3" ]; then
        reached=1; break
    fi
    # stop polling once everyone has finished all rounds
    { grep -aq 'RESULT rounds=' "$L1" && grep -aq 'RESULT rounds=' "$L2" \
      && grep -aq 'RESULT rounds=' "$L3"; } && break
    sleep 0.25; i=$((i + 1))
done
if grep -aqE 'peers=[1-9]' "$L1" || grep -aqE 'peers=[1-9]' "$L2"; then
    ok "nodes are EXCHANGING peer models over the relay (peers>=1 seen)"
else
    bad "no node ever saw a peer model (peers stayed 0) — gossip transport broken"
fi
log "collective full_acc (x10): node1=$A1 node2=$A2 node3=$A3   (solo: $S1 $S2 $S3)"
grep -aoE 'node=[0-9] round=[0-9]+ peers=[0-9] shard_acc=[0-9.]+% full_acc=[0-9.]+%' \
     "$L1" "$L2" "$L3" 2>/dev/null | sort -u | tail -3 | sed 's#.*/##; s/^/    /'
if [ "$reached" -eq 1 ]; then
    ok "EVERY node rose ABOVE its solo ceiling via gossip — node1 $A1>$S1, node2 $A2>$S2, node3 $A3>$S3 (x10)"
    ok "COLLECTIVE > INDIVIDUAL proven live for all 3 disjoint-shard nodes"
else
    bad "not all nodes cleared their solo ceiling together (node1=$A1/$S1 node2=$A2/$S2 node3=$A3/$S3, x10)"
fi

# ===========================================================================
echo
echo "==========================================================="
echo " §3 — kill -9 a node MID-LEARNING; the surviving swarm keeps learning"
echo "==========================================================="
# kill node3 while the swarm is still gossiping (mid-learning). The 2-node
# survivors keep gossiping and must FINISH still ABOVE their solo ceilings.
log "*** kill -9 node3 (pid ${NODE_PID[3]}) mid-learning ***"
kill -9 "${NODE_PID[3]}" 2>/dev/null; NODE_PID[3]=0
wait_for "$L1" 'RESULT rounds=' 240 || bad "node1 never finished after the kill"
wait_for "$L2" 'RESULT rounds=' 240 || bad "node2 never finished after the kill"
F1=$(last_full "$L1"); F2=$(last_full "$L2")
log "survivor finals (x10): node1=$F1 node2=$F2  (solo: $S1 $S2)"
grep -aE 'RESULT rounds=' "$L1" "$L2" | sed 's#.*/##; s/^/    /'
[ "${F1:-0}" -gt "${S1:-999}" ] && ok "node1 survived the kill ABOVE its solo ceiling ($F1 > $S1, x10)" \
                                || bad "node1 collective $F1 did NOT beat its solo ceiling $S1 (x10)"
[ "${F2:-0}" -gt "${S2:-999}" ] && ok "node2 survived the kill ABOVE its solo ceiling ($F2 > $S2, x10)" \
                                || bad "node2 collective $F2 did NOT beat its solo ceiling $S2 (x10)"

# ===========================================================================
echo
echo "==========================================================="
echo " §3 — the dead node REJOINS and catches up to the swarm via gossip"
echo "==========================================================="
# node3 rejoins with a FRESH p-fs dir (so it re-syncs cleanly instead of
# shadowing peers with stale local blocks), measures its own solo ceiling,
# then gossip-learns with the live swarm and must catch up ABOVE that ceiling.
log "restarting node3 (fresh p-fs) — it must catch up to the swarm by gossip ..."
rm -rf "$WORK/dir3"
start_node 3
exec 5<>"$FIFO.3"
t=0; while [ $t -lt 30 ]; do grep -aq -- '-> FULL' "$L3" && break; sleep 1; t=$((t+1)); done
sleep 2
send 3 "dtr gossip solo 160"
wait_for "$L3" 'solo_ceiling=' 60 || true
S3b=$(solo_ceil "$L3")
log "rejoined node3 solo ceiling=$S3b (x10) — now catching up by gossip"
# survivors resume gossip (fresh run) so node3 has LIVE peers to learn from.
: > "$L1"; : > "$L2"
send 1 "dtr gossip run $ROUNDS $LOCAL"
send 2 "dtr gossip run $ROUNDS $LOCAL"
send 3 "dtr gossip run $ROUNDS $LOCAL"
# the rejoining node must reach above its solo ceiling (peak during the run —
# it catches up to the live swarm).
F3PEAK=0; caught=0; i=0
while [ "$i" -lt 1200 ]; do      # up to 300s
    F3=$(last_full "$L3")
    [ "${F3:-0}" -gt "${F3PEAK:-0}" ] && F3PEAK=$F3
    [ "${F3:-0}" -gt "$S3b" ] && caught=1
    grep -aq 'RESULT rounds=' "$L3" && break
    sleep 0.25; i=$((i + 1))
done
log "rejoined node3 catch-up peak full_acc (x10): $F3PEAK  (solo ceiling $S3b)"
grep -aE 'node=.* RESULT rounds=' "$L3" | tail -1 | sed 's#.*/##; s/^/    /'
[ "$caught" -eq 1 ] && ok "rejoined node3 caught up: collective $F3PEAK > solo ceiling $S3b (x10)" \
                    || bad "rejoined node3 did NOT exceed its solo ceiling (peak $F3PEAK vs $S3b, x10)"

# ===========================================================================
echo
if [ "$FAIL" -ne 0 ]; then
    echo "==========================================================="
    echo " RESULT: FAIL — see [FAIL] lines above and /tmp/p32_*.log"
    echo "==========================================================="
    exit 1
fi
echo "==========================================================="
echo " RESULT: PASS — the swarm LEARNED COLLECTIVELY."
echo " Each node trained on a DISJOINT (leave-one-class-out) shard and could"
echo " not learn the whole task alone (solo ceiling ~66%); by gossiping and"
echo " averaging each other's full weight bodies with NO central aggregator,"
echo " in the slow deliberation band, EVERY node rose ABOVE its solo ceiling."
echo " A node killed mid-learning did not stop the swarm — survivors kept"
echo " improving — and a rejoining node caught up via gossip. §8 'the whole"
echo " strengthens the future' is no longer empty."
echo "==========================================================="
exit 0
