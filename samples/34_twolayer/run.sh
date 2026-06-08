#!/bin/bash
# ===========================================================================
# 34_twolayer / run.sh  —  THINKING CHANGES GUARDING (G38, wave 17).
# survival-network.md §8 "近傍が今を守り、全体が未来を強くする" / §9 考える器官.
#
# audit-7 §4.3 verdict: even with G22 collective learning landed, the learned
# model reached the guard layer by ZERO arrows — "二層は並んでいるだけで結合
# していない." G38 wires the two layers and PROVES learning makes guarding
# better.
#
#   Arrow 1 (learning -> guarding): moe_infer feeds the reflex the LEARNED
#     transformer's REAL max-softmax confidence + argmax class (was a dead
#     0xFF that always fired = G34). A low-confidence/unlearned input no longer
#     fires a spurious reflex; a learned-confident threat fires decisively.
#   Arrow 2 (guarding -> learning): the reflex's per-class threat experience
#     becomes a learning PRIORITY (slow deliberation band).
#
# WHAT THIS DEMONSTRATES, LIVE, OVER THE RELAY:
#   (0) in-process property tests [g38-confidence-live] /
#       [g38-learning-improves-guarding] / [g38-guard-feeds-learning].
#   (1) each node measures its UNLEARNED guard score (the learned model gates
#       the SAME reflex used live), then does G22 collective gossip learning
#       over the relay, then measures its LEARNED guard score — which must be
#       measurably HIGHER. (learning improved guarding, in numbers.)
#   (2) §3 survival: kill -9 a node MID-LEARNING; the surviving swarm finishes
#       and STILL GUARDS (its learned guard score stays above unlearned).
#
# HOST NOTE: on the aarch64-PRoot dev host cross-node p-fs crashes the native
# build, so (like sample 32 / G22) validate the LIVE path on x86_64 via:
#   PKERNEL_BOOT_DIR=boot/linux_x86_64 PKERNEL_WRAP=qemu-x86_64 samples/34_twolayer/run.sh
# In CI (x86_64 runner) both overrides are unset -> native build, no wrapper.
#
# Exit 0 = all PASS. Logs: /tmp/p34_*.log
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
[ -n "${PKERNEL_BOOT_DIR:-}" ] && BOOT="$ROOT/$PKERNEL_BOOT_DIR"
WRAP="${PKERNEL_WRAP:-}"
[ -x "$BOOT/p-kernel" ]    || make -C "$BOOT"        >/dev/null || exit 1
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay"  >/dev/null || exit 1

export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
export PKERNEL_RELAY_HOST=127.0.0.1
export PKERNEL_RELAY_PORT=7434

ROUNDS="${GL_ROUNDS:-24}"
LOCAL="${GL_LOCAL:-4}"

WORK="$(mktemp -d /tmp/p34_work.XXXXXX)"
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
        $WRAP "$BOOT/p-kernel" < "$FIFO.$i" > "/tmp/p34_node$i.log" 2>&1 &
    NODE_PID[$i]=$!; disown "${NODE_PID[$i]}"
    log "node$i up pid=${NODE_PID[$i]} log=/tmp/p34_node$i.log"
}

# guard_score for the LEARNED ('') or UNLEARNED ('fresh') line, as integer x10.
guard_x10() {  # <logfile> <LEARNED|UNLEARNED>
    # NOTE: anchor on a leading space — "UNLEARNED" contains "LEARNED" as a
    # substring, so an un-anchored "LEARNED guard_score=" also matched the
    # UNLEARNED line; with both lines in one log, tail -1 then non-deterministically
    # picked the wrong one (passed on qemu, FAILED on the hosted x86_64 runner).
    grep -aoE "[[:space:]]$2 guard_score=[0-9]+\.[0-9]" "$1" 2>/dev/null | tail -1 \
        | grep -aoE '[0-9]+\.[0-9]' | tr -d .
}
wait_for() {  # <file> <pattern> <secs>
    local f="$1" p="$2" s="$3" i=0
    while [ "$i" -lt "$((s * 4))" ]; do
        grep -aqE "$p" "$f" 2>/dev/null && return 0
        sleep 0.25; i=$((i + 1))
    done
    return 1
}

L1=/tmp/p34_node1.log
L2=/tmp/p34_node2.log
L3=/tmp/p34_node3.log

# ===========================================================================
echo "==========================================================="
echo " (0) in-kernel property self-test: thinking changes guarding (G38)"
echo "==========================================================="
ST=/tmp/p34_selftest.log
printf 'dtr gossip g38\nexit\n' | timeout 200 $WRAP "$BOOT/p-kernel" > "$ST" 2>&1 || true
grep -aE '\[g38(-confidence-live|-learning-improves-guarding|-guard-feeds-learning)?\]' "$ST" | sed 's/^/    /'
grep -qaE '\[g38-confidence-live\] PASS'           "$ST" && ok "[g38-confidence-live] PASS"           || bad "[g38-confidence-live] failed"
grep -qaE '\[g38-learning-improves-guarding\] PASS' "$ST" && ok "[g38-learning-improves-guarding] PASS" || bad "[g38-learning-improves-guarding] failed"
grep -qaE '\[g38-guard-feeds-learning\] PASS'      "$ST" && ok "[g38-guard-feeds-learning] PASS"      || bad "[g38-guard-feeds-learning] failed"

# ===========================================================================
echo
echo "==========================================================="
echo " LIVE — 3 nodes, DISJOINT shards: collective learning improves GUARDING"
echo "==========================================================="
log "--- relay :$PKERNEL_RELAY_PORT + 3 nodes (each leave-one-class-out) ---"
"$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" -v > "$WORK/relay.log" 2>&1 &
RELAY_PID=$!; disown "$RELAY_PID"; sleep 1
for i in 1 2 3; do start_node "$i"; done
exec 3<>"$FIFO.1" 4<>"$FIFO.2" 5<>"$FIFO.3"

t=0; while [ $t -lt 40 ]; do grep -aq -- '-> FULL' "$L1" && break; sleep 1; t=$((t+1)); done
[ $t -ge 40 ] && { bad "cluster never reached FULL"; echo "RESULT: FAIL"; exit 1; }
log "cluster FULL after ~${t}s"; sleep 2

# --- 1) each node's UNLEARNED guard score (baseline) -----------------------
log "measuring each node's UNLEARNED guard score (learned model gates the reflex) ..."
send 1 "dtr gossip guard fresh"; send 2 "dtr gossip guard fresh"; send 3 "dtr gossip guard fresh"
for L in "$L1" "$L2" "$L3"; do wait_for "$L" 'UNLEARNED guard_score=' 60 || bad "no UNLEARNED guard on $L"; done
U1=$(guard_x10 "$L1" UNLEARNED); U2=$(guard_x10 "$L2" UNLEARNED); U3=$(guard_x10 "$L3" UNLEARNED)
log "UNLEARNED guard (x10): node1=$U1 node2=$U2 node3=$U3"
grep -aE 'UNLEARNED guard_score=' "$L1" "$L2" "$L3" | sed 's#.*/##; s/^/    /'

# --- 2) collective gossip learning over the relay (no central aggregator) ---
log "collective gossip learning (rounds=$ROUNDS local=$LOCAL) — the slow §8 band ..."
send 1 "dtr gossip run $ROUNDS $LOCAL"
send 2 "dtr gossip run $ROUNDS $LOCAL"
send 3 "dtr gossip run $ROUNDS $LOCAL"

# let several rounds land + peers exchange, then kill a node MID-LEARNING (§3).
# (the live drpc node id is 0-based in the [g22-live] line, so match round-only.)
wait_for "$L1" 'round=4 ' 240 || bad "node1 not learning"
if grep -aqE 'peers=[1-9]' "$L1" || grep -aqE 'peers=[1-9]' "$L2"; then
    ok "nodes are EXCHANGING peer models over the relay (peers>=1 seen)"
else
    bad "no node ever saw a peer model (peers stayed 0) — gossip transport broken"
fi

echo
echo "==========================================================="
echo " §3 — kill -9 a node MID-LEARNING; the swarm still GUARDS"
echo "==========================================================="
log "*** kill -9 node3 (pid ${NODE_PID[3]}) mid-learning ***"
kill -9 "${NODE_PID[3]}" 2>/dev/null; NODE_PID[3]=0
wait_for "$L1" 'RESULT rounds=' 300 || bad "node1 never finished after the kill"
wait_for "$L2" 'RESULT rounds=' 300 || bad "node2 never finished after the kill"
ok "survivors node1,node2 finished gossip-learning after the kill — the swarm survived"

# --- 3) survivors' LEARNED guard score must beat their UNLEARNED baseline ---
log "measuring survivors' LEARNED guard score (after collective learning + a death) ..."
send 1 "dtr gossip guard"; send 2 "dtr gossip guard"
for L in "$L1" "$L2"; do wait_for "$L" 'LEARNED guard_score=' 60 || bad "no LEARNED guard on $L"; done
G1=$(guard_x10 "$L1" LEARNED); G2=$(guard_x10 "$L2" LEARNED)
log "LEARNED guard (x10): node1=$G1 node2=$G2   (unlearned: $U1 $U2)"
grep -aE 'LEARNED guard_score=' "$L1" "$L2" | sed 's#.*/##; s/^/    /'

if [ "${G1:-0}" -gt "${U1:-999}" ]; then
    ok "node1: LEARNING IMPROVED GUARDING live ($G1 > $U1 x10) — survived the kill, still guards"
else
    bad "node1 learned guard $G1 did not beat unlearned $U1 (x10)"
fi
if [ "${G2:-0}" -gt "${U2:-999}" ]; then
    ok "node2: LEARNING IMPROVED GUARDING live ($G2 > $U2 x10) — survived the kill, still guards"
else
    bad "node2 learned guard $G2 did not beat unlearned $U2 (x10)"
fi

# ===========================================================================
echo
if [ "$FAIL" -ne 0 ]; then
    echo "==========================================================="
    echo " RESULT: FAIL — see [FAIL] lines above and /tmp/p34_*.log"
    echo "==========================================================="
    exit 1
fi
echo "==========================================================="
echo " RESULT: PASS — THINKING CHANGES GUARDING."
echo " The learned transformer's REAL confidence gates the reflex (arrow 1),"
echo " the reflex's threat experience prioritizes the learning (arrow 2), and"
echo " over the relay collective learning measurably raised each node's GUARD"
echo " score above its unlearned baseline — surviving a node killed mid-flight."
echo " §8 'the whole strengthens the future' now reaches 'the near guards now.'"
echo "==========================================================="
exit 0
