#!/bin/bash
# ===========================================================================
# 46_belief_revision / run.sh — BELIEF REVISION over the mesh
#   (living-mind-lm12-belief-revision.md, LM-12; the 41_shared_mind shape)
#
# A belief that is ALREADY weight-resident on both nodes is REVISED, and the
# new belief DISPLACES the old across the region — not blended, not masked.
#
# Node roles (chosen so the LM-12 Site-3 stale-mouth guard is LOAD-BEARING,
# not decorative — see [rev-stale-mouth] below):
#   A  teaches sun->yellow LOCALLY (Site 1). This ARMS A's mouth to "yellow"
#      (m_publish_teach sets mt_pub_last=yellow, mt_pub_have=1); A re-drives it
#      every poll so late region members still receive it.
#   B  learns sun->yellow REMOTELY (Site 2, mind_net_task -> r3_fact_learn).
#      Site 2 NEVER arms the mouth, so B never re-drives "yellow".
#   B  then REVISES sun->green LOCALLY (Site 1). B's mouth arms to "green".
#   A  takes B's revision as a REMOTE revision (Site 2). This is the guard-
#      critical state: A's mouth is still armed to the SUPERSEDED "yellow"
#      while its queue is being revised to "green". The Site-3 guard
#      (r3_incontext.c, r3_fact_revise: "if (mt_pub_have && key==k &&
#      val!=v_new) mt_pub_have=0;") MUST clear A's stale mouth here, because
#      Site 2 will NOT re-arm it — nothing else stops A re-driving "yellow".
#
#   1. teach sun->yellow on A ; B learns it (Path E) and answers "yellow".
#   2. `mind teach sun green` on B = LM-12 belief revision (Site 1): B
#      SUPERSEDES its own binding in place and re-publishes the NEW value.
#   3. A's mind_net_task takes the different-value packet as a REMOTE REVISION
#      (Site 2, last-arrival-wins), supersedes in place, fires the Site-3
#      stale-mouth guard, and A's OWN DMN re-grounds A's rw[] on "green".
#      A now answers "green" (>= 75% masked).
#   4. [rev-stale-mouth] (THE TEETH): a FRESH late-joiner witness node Z
#      joins the region AFTER the revision has settled. Because K-DDS
#      "mind/teach" is single-slot LATEST_ONLY with per-origin (origin,seq)
#      dedup, an EXISTING member that already saw A's "yellow" seq would
#      DEDUP a stale re-drive — so re-infection is observable ONLY by a fresh
#      joiner with empty dedup state. Z must converge on "green" and must
#      NEVER bind "yellow" over >= 3 poll cycles. If the Site-3 guard is
#      removed, A keeps re-driving the superseded "yellow" and Z IS re-
#      infected (its dedup is empty) — this gate then FAILS. A is proven
#      region-reachable to Z (A's region size includes Z), so Z's cleanliness
#      is the guard's doing, not an unreachable path.
#   5. kill -9 B (the reviser) ; A (and Z) STILL answer "green" — the revised
#      belief outlived its reviser (the Collective kept the correction).
#
# Two tags:
#   [rev-live]         teach->answer, revise->answer-new, kill-reviser->still-new
#   [rev-stale-mouth]  a FRESH late joiner converges on the new belief and is
#                      NOT re-infected with the old one (the Site-3 guard bites)
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

L1=/tmp/p46_nodeA.log    # A = teacher of VOLD; takes the remote revision (guard-critical); region 0
L2=/tmp/p46_nodeB.log    # B = learner then REVISER (the reviser we kill); region 0
L3=/tmp/p46_nodeZ.log    # Z = FRESH LATE JOINER witness (joins after the revision settles)

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

start_node() {  # <i> <fifo> <dir> <log>
    local i="$1" fifo="$2" dir="$3" lg="$4"
    rm -f "$fifo"; mkfifo "$fifo"; mkdir -p "$dir"
    env PKERNEL_NODE_ID=$i PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$dir" \
        $WRAP "$BOOT/p-kernel" < "$fifo" > "$lg" 2>&1 &
    NODE_PID[$i]=$!; disown "${NODE_PID[$i]}"
    log "node$i up pid=${NODE_PID[$i]} dir=$dir log=$lg"
}
send() {  # <node 1|2|3> <line...> — node 1->fd 3 (A), 2->fd 4 (B), 3->fd 5 (Z)
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
echo " LM-12 — belief revision over the mesh: teach@A -> revise@B -> A corrects -> fresh joiner Z stays clean"
echo "==========================================================="
"$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" > "$WORK/relay.log" 2>&1 &
RELAY_PID=$!; disown "$RELAY_PID"; sleep 1

# A and B start now; Z (node 3) joins LATE (after the revision settles), so the
# teeth witness has EMPTY per-origin dedup state at the moment A's stale re-drive
# would strike. Its fifo is created here so exec can open the fd once it starts.
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
# Phase 1: establish the shared belief sun->yellow. A teaches it LOCALLY (Site 1,
# arming A's mouth to "yellow"); B learns it REMOTELY (Site 2, mouth NOT armed).
# ---------------------------------------------------------------------------
echo
echo "--- Phase 1: teach sun->$VOLD on A (Site 1, arms A's mouth); B learns it (Site 2) ---"
send 1 "mind teach $KWORD $VOLD"
wait_for "$L1" 'published mind/teach' 200 || bad "A never published the teach"
wait_for "$L2" '\[shared-arrival\] PASS' 200 || bad "B never received A's teach"
send 2 "mind wait 90"
wait_for "$L2" 'distilled in-context facts -> rw\[\]' 200 \
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
# Phase 2: REVISE sun->green on B (Site 1). A takes it as a REMOTE revision
# (Site 2) while A's mouth is STILL armed to "yellow" — the guard-critical state.
# ---------------------------------------------------------------------------
echo
echo "--- Phase 2: revise sun->$VNEW on B (Site 1); A takes the REMOTE REVISION (Site 2, mouth-armed-to-$VOLD) ---"
REVISE_T=$(date +%s)
send 2 "mind teach $KWORD $VNEW"       # B already-bound key => LM-12 revision (Site 1)
wait_for "$L2" 'REVISE key .* "'"$VOLD"'"->"'"$VNEW"'"' 60 \
    || bad "B did not enter the belief-revision branch (Site 1)"
wait_for "$L2" '\[revise-arrival\] PASS' 60 || bad "B's [revise-arrival] did not PASS"
# A applies the remote revision (Site 2, last-arrival-wins); this is where the
# Site-3 stale-mouth guard fires (A's mouth was armed to the now-superseded VOLD).
if wait_for "$L1" '\[shared-revise\] PASS' 200; then
    ok "A applied the REMOTE REVISION (Site 2): last-arrival-wins — Site-3 guard fired here"
    grep -a 'remote REVISE key' "$L1" | tail -1 | sed 's/^/    /'
else
    bad "A never took the remote revision ([shared-revise] absent)"
fi
# A's OWN DMN re-grounds rw[] on the new belief.
send 1 "mind wait 90"
wait_for "$L1" 'wait: drained' 120 || true
if ask_fresh 1 "$L1" 90 && echo "$FRESH_ANS" | grep -aq "\"$KWORD\" -> \"$VNEW\""; then
    S=$(share_of "$FRESH_ANS")
    if [ "${S:-0}" -ge "$SHARE_GATE" ]; then
        ok "A now answers \"$KWORD\" -> \"$VNEW\" (share=${S}%) — the belief was REVISED, not blended"
    else
        bad "A's revised-belief share ${S:-?}% < $SHARE_GATE"
    fi
    echo "    revise@B=$REVISE_T  answer@A=$(date +%s)  delta=$(( $(date +%s) - REVISE_T ))s"
    echo "    $FRESH_ANS"
else
    bad "A did not answer \"$VNEW\" for \"$KWORD\" after the revision"
    echo "    ${FRESH_ANS:-<none>}"
fi

# ---------------------------------------------------------------------------
# [rev-stale-mouth] (THE TEETH): a FRESH late joiner Z must converge on the NEW
# belief and must NEVER bind the OLD one. With the Site-3 guard, A cleared its
# stale mouth in Phase 2 and is now SILENT on mind/teach — only B re-drives
# "green". Without the guard, A keeps re-driving the superseded "yellow", and Z
# (empty per-origin dedup) IS re-infected: this gate FAILS.
# ---------------------------------------------------------------------------
echo
echo "--- [rev-stale-mouth]: FRESH late joiner Z must converge on \"$VNEW\" and never bind \"$VOLD\" ---"
STALE_OK=1
start_node 3 "$WORK/fZ" "$WORK/dZ" "$L3"
exec 5<>"$WORK/fZ"
wait_for "$L3" 'mind_net_task up' 90 || bad "Z's mind_net_task never started"

# Prove the delivery path A->Z is OPEN: A must see Z in its region (size=3). If
# A's mouth were still armed to "yellow" (guard removed), A WOULD deliver it to Z.
log "waiting for A to see Z in its region (size=3) — proves A->Z path is reachable ..."
t=0; r3=0
while [ $t -lt 150 ]; do
    printf 'region\n' >&3; sleep 2
    if grep -aq 'size=3' "$L1"; then r3=1; break; fi
    t=$((t+2))
done
[ "$r3" -eq 1 ] && ok "A's region includes Z (size=3) — the A->Z delivery path is open" \
                || { bad "A never saw Z in its region (size!=3) — teeth precondition unmet"; STALE_OK=0; }

# Z converges on the NEW belief via the region (B re-drives "green").
if wait_for "$L3" 'remote (teach arrived: "'"$KWORD"'"->"'"$VNEW"'"|REVISE key.*->"'"$VNEW"'")' 200; then
    ok "Z received the NEW belief \"$VNEW\" from the region"
    grep -aE 'remote (teach arrived|REVISE key)' "$L3" | tail -1 | sed 's/^/    /'
else
    bad "Z never received the new belief \"$VNEW\""; STALE_OK=0
fi

# Settle >= 3 poll cycles (MT_POLL_MS=500ms). A re-drives its retained mouth every
# poll; with the guard it is CLEARED (silent), so Z sees only "green". Keep poking
# A's region so it stays scheduled and (guard removed) keeps re-driving to Z.
log "settling >= 3 poll cycles (both re-drive every 500ms); watching Z for stale \"$VOLD\" ..."
for _ in 1 2 3 4 5 6 7 8; do printf 'region\n' >&3; sleep 2; done   # ~16s >> 3 poll cycles

# THE TEETH: a fresh joiner must NEVER bind "$VOLD". Any remote line landing on
# VOLD (arrival "...->\"yellow\"" OR revise "...->\"yellow\"") is a re-infection.
if grep -aqE 'remote (teach arrived: "'"$KWORD"'"|REVISE key).*->"'"$VOLD"'"' "$L3"; then
    bad "Z was RE-INFECTED with the OLD belief \"$VOLD\" (Site-3 stale-mouth guard leaked)"
    grep -aE 'remote (teach arrived: "'"$KWORD"'"|REVISE key).*->"'"$VOLD"'"' "$L3" | tail -1 | sed 's/^/    (stale) /'
    STALE_OK=0
else
    ok "Z never bound \"$VOLD\" over >= 3 poll cycles — no stale re-infection"
fi

# Converge: Z's OWN DMN grounds "green"; a masked ask on Z yields "$VNEW".
send 3 "mind wait 90"
wait_for "$L3" 'distilled in-context facts -> rw\[\]' 200 || true
wait_for "$L3" 'wait: drained' 30 || true
if ask_fresh 3 "$L3" 90 && echo "$FRESH_ANS" | grep -aq "\"$KWORD\" -> \"$VNEW\""; then
    S=$(share_of "$FRESH_ANS")
    ok "Z answers \"$KWORD\" -> \"$VNEW\" (share=${S:-?}%) — the fresh joiner converged on the revised belief"
    echo "    $FRESH_ANS"
else
    bad "Z did not converge on \"$VNEW\""; STALE_OK=0
    echo "    ${FRESH_ANS:-<none>}"
fi
[ "$STALE_OK" -eq 1 ] && ok "[rev-stale-mouth] PASS" || bad "[rev-stale-mouth] — see above"

# ---------------------------------------------------------------------------
# kill -9 B (the reviser): the revised belief must outlive its reviser.
# ---------------------------------------------------------------------------
echo
echo "--- kill -9 B (the reviser dies); A must STILL answer \"$VNEW\" ---"
log "*** kill -9 node B (pid ${NODE_PID[2]}) ***"
kill -9 "${NODE_PID[2]}" 2>/dev/null; NODE_PID[2]=0
sleep 3
LIVE_OK=0
if ask_fresh 1 "$L1" 90; then
    S=$(share_of "$FRESH_ANS")
    if echo "$FRESH_ANS" | grep -aq "\"$KWORD\" -> \"$VNEW\"" && [ "${S:-0}" -ge "$SHARE_GATE" ]; then
        ok "A STILL answers \"$VNEW\" (share=${S}%) after B's death — the correction survived its reviser"
        LIVE_OK=1
    else
        bad "A failed to answer \"$VNEW\" after B's death (line: $FRESH_ANS)"
    fi
    echo "    after B's death: $FRESH_ANS"
else
    bad "A printed no fresh ask line after B's death"
fi
# and the fresh joiner Z, too, still answers the revised belief.
if ask_fresh 3 "$L3" 60 && echo "$FRESH_ANS" | grep -aq "\"$KWORD\" -> \"$VNEW\""; then
    ok "Z also STILL answers \"$VNEW\" after B's death — the Collective (A+Z) kept the correction"
    echo "    after B's death (Z): $FRESH_ANS"
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
echo " A belief taught on A (sun->$VOLD) was learned on B; a revise on B"
echo " (sun->$VNEW) DISPLACED it on A through A's OWN DMN sleep (Site 2 remote"
echo " revision, last-arrival-wins). The Site-3 stale-mouth guard cleared A's"
echo " superseded mouth, so a FRESH late joiner Z converged on the new belief"
echo " and was NEVER re-infected with the old one; killing the reviser B did"
echo " not un-correct the region."
echo "==========================================================="
exit 0
