#!/bin/bash
# ===========================================================================
# 46_belief_revision / run.sh — BELIEF REVISION over the mesh
#   (living-mind-lm12-belief-revision.md, LM-12; the 41_shared_mind shape)
#
# A belief that is ALREADY weight-resident on both nodes is REVISED, and the
# new belief DISPLACES the old across the region — not blended, not masked.
#
#   1. teach sun->yellow on A ; B learns it (Path E) and answers "yellow".
#   2. `mind teach sun green` on A = LM-12 belief revision (Site 1): A
#      SUPERSEDES its own binding in place and re-publishes the NEW value.
#   3. B's mind_net_task takes the different-value packet as a REMOTE REVISION
#      (Site 2, last-arrival-wins), supersedes in place, and B's OWN DMN
#      re-grounds B's rw[] on "green".  B now answers "green" (>= 75% masked).
#   4. kill -9 A ; B STILL answers "green" — the revised belief outlived its
#      reviser (the Collective kept the correction).
#   5. [rev-stale-mouth]: after >= 3 poll cycles post-revision, neither node
#      reverts to "yellow" — the Site 3 stale-re-drive guard cleared A's
#      retained OLD teach so a late poll cannot re-infect the region.
#
# Two tags:
#   [rev-live]         teach->answer, revise->answer-new, kill-reviser->still-new
#   [rev-stale-mouth]  no reversion to the old belief after the revision settles
#
# Exit 0 = RESULT: PASS. Logs: /tmp/p46_*.log
# ===========================================================================
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "unsupported host arch $(uname -m)"; exit 1 ;;
esac
[ -n "${PKERNEL_BOOT_DIR:-}" ] && BOOT="$ROOT/$PKERNEL_BOOT_DIR"
WRAP="${PKERNEL_WRAP:-}"
[ -x "$BOOT/p-kernel" ]    || make -C "$BOOT"       >/dev/null || exit 1
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay" >/dev/null || exit 1

export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
export PKERNEL_RELAY_HOST=127.0.0.1
export PKERNEL_RELAY_PORT="${PKERNEL_RELAY_PORT:-7461}"

KWORD=sun           # key token id 2
VOLD=yellow         # the first belief  (answer token id 3)
VNEW=green          # the revised belief (answer token id 1)
SHARE_GATE=75       # the LM-6 bar

WORK="$(mktemp -d /tmp/p46_work.XXXXXX)"
declare -A NODE_PID
RELAY_PID=0
FAIL=0

TS()   { date '+%H:%M:%S'; }
log()  { echo "[$(TS)] $*"; }
ok()   { log "ok  : $*"; }
bad()  { log "FAIL: $*"; FAIL=1; }

L1=/tmp/p46_nodeA.log    # A = teacher + reviser (region 0)
L2=/tmp/p46_nodeB.log    # B = learner  (region 0)

cleanup() {
    exec 3>&- 4>&- 2>/dev/null || true
    for i in 1 2; do
        [ "${NODE_PID[$i]:-0}" != 0 ] && kill -9 "${NODE_PID[$i]}" 2>/dev/null
        NODE_PID[$i]=0
    done
    [ "$RELAY_PID" != 0 ] && kill -9 "$RELAY_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

start_node() {  # <i> <fifo> <dir> <log>
    local i="$1" fifo="$2" dir="$3" lg="$4"
    rm -f "$fifo"; mkfifo "$fifo"; mkdir -p "$dir"
    env PKERNEL_NODE_ID=$i PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$dir" \
        $WRAP "$BOOT/p-kernel" < "$fifo" > "$lg" 2>&1 &
    NODE_PID[$i]=$!; disown "${NODE_PID[$i]}"
    log "node$i up pid=${NODE_PID[$i]} dir=$dir log=$lg"
}
send() {  # <node 1|2> <line...>
    local node="$1"; shift
    local fd=$((node + 2))
    log "node$node <- '$*'"
    printf '%s\n' "$*" >&"$fd"
}
wait_for() {  # <file> <pattern> <secs>
    local f="$1" p="$2" s="$3" i=0
    while [ "$i" -lt "$((s * 4))" ]; do
        grep -aqE "$p" "$f" 2>/dev/null && return 0
        sleep 0.25; i=$((i + 1))
    done
    return 1
}
# ask <node> and break the INSTANT a STRICTLY-NEW ask line for KWORD appears,
# returning that fresh line's answer word via the FRESH_ANS global.
FRESH_ANS=""
ask_fresh() {  # <node> <log> <bound-secs>
    local node="$1" lg="$2" bound="$3"
    local pre; pre=$(grep -ac "ask \"$KWORD\"" "$lg")
    local got=0 tries=$((bound / 2))
    for _ in $(seq 1 "$tries"); do
        send "$node" "mind ask $KWORD"
        for _ in $(seq 1 8); do
            if [ "$(grep -ac "ask \"$KWORD\"" "$lg")" -gt "$pre" ]; then got=1; break; fi
            sleep 0.25
        done
        [ "$got" -eq 1 ] && break
    done
    FRESH_ANS=$(grep -a "ask \"$KWORD\"" "$lg" | tail -1)
    [ "$got" -eq 1 ]
}
share_of() { echo "$1" | grep -aoE 'share=[0-9]+\.[0-9]' | grep -aoE '[0-9]+\.[0-9]' | cut -d. -f1; }

echo "==========================================================="
echo " LM-12 — belief revision over the mesh: teach@A -> revise@A -> B corrects"
echo "==========================================================="
"$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" > "$WORK/relay.log" 2>&1 &
RELAY_PID=$!; disown "$RELAY_PID"; sleep 1

start_node 1 "$WORK/fA" "$WORK/dA" "$L1"
start_node 2 "$WORK/fB" "$WORK/dB" "$L2"
exec 3<>"$WORK/fA" 4<>"$WORK/fB"

wait_for "$L2" 'mind_net_task up' 60 || bad "B's mind_net_task never started"

# A,B form ONE region (RTT <= REGION_TAU_MS). Poll A's `region` verb.
log "waiting for A,B to form ONE region ..."
t=0; rok=0
while [ $t -lt 150 ]; do
    printf 'region\n' >&3; sleep 2
    if grep -aq 'size=2' "$L1"; then rok=1; break; fi
    t=$((t+2))
done
[ "$rok" -eq 1 ] && ok "A,B formed one region (size=2)" \
                 || bad "A,B never formed a region"

# ---------------------------------------------------------------------------
# Phase 1: establish the shared belief sun->yellow on BOTH nodes.
# ---------------------------------------------------------------------------
echo
echo "--- Phase 1: teach sun->$VOLD on A; B learns it (Path E) ---"
send 1 "mind teach $KWORD $VOLD"
wait_for "$L1" 'published mind/teach' 200 || bad "A never published the teach"
wait_for "$L2" '\[shared-arrival\] PASS' 150 || bad "B never received A's teach"
send 2 "mind wait 90"
wait_for "$L2" 'distilled in-context facts -> rw\[\]' 180 \
    || bad "B's DMN never consolidated the first belief"
wait_for "$L2" 'wait: drained' 30 || true
if ask_fresh 2 "$L2" 60 && echo "$FRESH_ANS" | grep -aq "\"$KWORD\" -> \"$VOLD\""; then
    S=$(share_of "$FRESH_ANS")
    if [ "${S:-0}" -ge "$SHARE_GATE" ]; then
        ok "B answers \"$KWORD\" -> \"$VOLD\" (share=${S}%) — the shared belief is live"
    else
        bad "B's first-belief share ${S:-?}% < $SHARE_GATE"
    fi
    echo "    $FRESH_ANS"
else
    bad "B did not answer \"$VOLD\" for \"$KWORD\" after Phase 1"
    echo "    ${FRESH_ANS:-<none>}"
fi

# ---------------------------------------------------------------------------
# Phase 2: REVISE sun->green on A (Site 1); B corrects itself (Site 2 + DMN).
# ---------------------------------------------------------------------------
echo
echo "--- Phase 2: revise sun->$VNEW on A; B takes the REMOTE REVISION ---"
REVISE_T=$(date +%s)
send 1 "mind teach $KWORD $VNEW"       # already-bound key => LM-12 revision
wait_for "$L1" 'REVISE key .* "'"$VOLD"'"->"'"$VNEW"'"' 60 \
    || bad "A did not enter the belief-revision branch (Site 1)"
wait_for "$L1" '\[revise-arrival\] PASS' 60 || bad "A's [revise-arrival] did not PASS"
# B applies the remote revision (Site 2, last-arrival-wins), printed loudly.
if wait_for "$L2" '\[shared-revise\] PASS' 150; then
    ok "B applied the REMOTE REVISION (Site 2): last-arrival-wins"
    grep -a 'remote REVISE key' "$L2" | tail -1 | sed 's/^/    /'
else
    bad "B never took the remote revision ([shared-revise] absent)"
fi
# B's OWN DMN re-grounds rw[] on the new belief.
send 2 "mind wait 90"
wait_for "$L2" 'wait: drained' 120 || true
if ask_fresh 2 "$L2" 90 && echo "$FRESH_ANS" | grep -aq "\"$KWORD\" -> \"$VNEW\""; then
    S=$(share_of "$FRESH_ANS")
    if [ "${S:-0}" -ge "$SHARE_GATE" ]; then
        ok "B now answers \"$KWORD\" -> \"$VNEW\" (share=${S}%) — the belief was REVISED, not blended"
    else
        bad "B's revised-belief share ${S:-?}% < $SHARE_GATE"
    fi
    echo "    revise@A=$REVISE_T  answer@B=$(date +%s)  delta=$(( $(date +%s) - REVISE_T ))s"
    echo "    $FRESH_ANS"
else
    bad "B did not answer \"$VNEW\" for \"$KWORD\" after the revision"
    echo "    ${FRESH_ANS:-<none>}"
fi

# ---------------------------------------------------------------------------
# [rev-stale-mouth]: >= 3 poll cycles later, NO reversion to the old belief.
# ---------------------------------------------------------------------------
echo
echo "--- [rev-stale-mouth]: settle >= 3 poll cycles; neither node reverts to \"$VOLD\" ---"
# A re-drives its retained teach every MT_POLL_MS (500ms). The Site 3 guard
# cleared the STALE "$VOLD" packet, so A now re-drives only "$VNEW". Poke A a
# few times and confirm B does not re-arrive/revert to the old value.
B_REV_BEFORE=$(grep -ac 'remote REVISE key' "$L2")
for _ in 1 2 3 4 5; do send 1 "mind"; sleep 2; done   # >= 3 poll cycles of re-drive
STALE_OK=1
# B must NOT have printed a fresh remote arrival/revision back to "$VOLD".
if grep -aqE "remote (teach arrived|REVISE key).*\"$VNEW\"->\"$VOLD\"" "$L2"; then
    bad "B reverted to the OLD belief on a stale re-drive (Site 3 guard leaked)"; STALE_OK=0
fi
# and a fresh ask on B still yields "$VNEW".
if ask_fresh 2 "$L2" 40 && echo "$FRESH_ANS" | grep -aq "\"$KWORD\" -> \"$VNEW\""; then
    ok "B still answers \"$VNEW\" after the region settled (no stale re-infection)"
else
    bad "B did not still answer \"$VNEW\" after settling"; STALE_OK=0
    echo "    ${FRESH_ANS:-<none>}"
fi
[ "$STALE_OK" -eq 1 ] && ok "[rev-stale-mouth] PASS" || bad "[rev-stale-mouth] — see above"

# ---------------------------------------------------------------------------
# kill -9 A: the revised belief must outlive its reviser.
# ---------------------------------------------------------------------------
echo
echo "--- kill -9 A (the reviser dies); B must STILL answer \"$VNEW\" ---"
log "*** kill -9 node A (pid ${NODE_PID[1]}) ***"
kill -9 "${NODE_PID[1]}" 2>/dev/null; NODE_PID[1]=0
sleep 3
LIVE_OK=0
if ask_fresh 2 "$L2" 90; then
    S=$(share_of "$FRESH_ANS")
    if echo "$FRESH_ANS" | grep -aq "\"$KWORD\" -> \"$VNEW\"" && [ "${S:-0}" -ge "$SHARE_GATE" ]; then
        ok "B STILL answers \"$VNEW\" (share=${S}%) after A's death — the correction survived"
        LIVE_OK=1
    else
        bad "B failed to answer \"$VNEW\" after A's death (line: $FRESH_ANS)"
    fi
    echo "    after A's death: $FRESH_ANS"
else
    bad "B printed no fresh ask line after A's death"
fi

[ "$LIVE_OK" -eq 1 ] && [ "$STALE_OK" -eq 1 ] && ok "[rev-live] PASS — teach->revise->correct->survive-death" \
    || bad "[rev-live] — see [FAIL] lines above"

# ===========================================================================
echo
if [ "$FAIL" -ne 0 ]; then
    echo "==========================================================="
    echo " RESULT: FAIL — see [FAIL] lines above and /tmp/p46_*.log"
    echo "==========================================================="
    exit 1
fi
echo "==========================================================="
echo " RESULT: PASS — BELIEF REVISION IS REAL ACROSS THE MESH."
echo " A belief taught on A (sun->$VOLD) was answered on B; a later revise on A"
echo " (sun->$VNEW) DISPLACED it on B through B's OWN DMN sleep (Site 2 remote"
echo " revision, last-arrival-wins); the region did not revert to the old value"
echo " on stale re-drives; and killing the reviser did not un-correct B."
echo "==========================================================="
exit 0
