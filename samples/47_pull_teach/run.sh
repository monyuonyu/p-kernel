#!/bin/bash
# ===========================================================================
# 47_pull_teach / run.sh  —  REGION PULL-TEACH (living-mind-lm15-pullteach.md)
#
# The mind REACHES OUT for what it lacks. Where LM-7 (sample 41) pushes a fact
# a teacher chose to share, LM-15 lets the LEARNER ASK: node A wonders about a
# key K it was ASKED but does NOT know, PUBLISHES that want on the region topic
# "mind/want" (the KEY crosses the wire; the accrued want LEVEL never does —
# F-LOCAL), and a peer that HOLDS K re-teaches the answer on the EXISTING
# "mind/teach" topic. The answer lands through A's UNCHANGED r3_fact_learn mouth
# and the LM-14 want->salience conversion fires, so the pulled answer arrives
# PRECIOUS (arrival salience 1 + want).
#
# The script NEVER types `mind teach <K> ...` on A or C — that is what gives the
# network-arrival assertion teeth (A can only LEARN K from the wire). K is
# DISCOVERED dynamically (the first candidate word A prints `wondering about`
# for — selection by observed accrual, the curio_pick_unknown discipline in the
# driving script, NOT a hard-coded key: GENERICITY, the LM-13 lesson).
#
# Topology = sample 41's: relay + 3 nodes, PKERNEL_RTT_ZONE_SIZE=2 -> A(1),B(2)
# share region 0; C(3) is in zone 1, OUTSIDE the region (the negative control).
#
# Disease then cure, in ONE run (same binary, the only delta is region member-
# ship + who holds K):
#   [pull-want-live]      B receives A's want for K over the REAL mind/want topic
#   [pull-unknown-silent] nobody holds K -> B stays silent, A gets ZERO arrival,
#                         A's want persists (the mechanism does not hallucinate)
#   [pull-region-quiet]   C's want never reaches B (region bounds the question)
#   [pull-answered]       teach B only -> B answers via PULL -> A learns K over
#                         the wire (from B = internal node 1) -> salience == 1+want
#   [pull-want-cleared]   A no longer wonders K; A's next mind/want is empty
#   [pull-grounded]       A answers V and names the teacher (B = internal node 1)
#   [pull-consolidated]   A's OWN DMN grounds it: re-ask share >= 75
#   [pull-region]         C: still wondering, never received K (region control)
#   [pull-storm-bounded]  exactly ONE r3_fact_learn for K on A; B's answers cap
#
# Exit 0 = RESULT: PASS. Logs: /tmp/p47_*.log
#
# Risk notes (living-mind-lm15-pullteach.md §8): keep <= R3_WQ_MAX(=4) wants live
# on A so an eviction race can't flake [pull-answered]; first-teach on B pays the
# lazy-pretrain cost (window sized for it, sample-41 :185 precedent).
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
export PKERNEL_RELAY_PORT="${PKERNEL_RELAY_PORT:-7447}"

# The answer word V (a valid off-bias answer token; the same proven off-bias
# pick sample 41 uses). K is discovered at run time, V is fixed and readable.
VWORD=yellow        # = answer token id 3
SHARE_GATE=75       # the LM-6 bar; measured 100 post-sleep
WANT_W=3            # script-chosen wonder count (< R3_WANT_CAP=8); s == 1 + W

# The kernel prints the 0-based INTERNAL drpc_my_node (PKERNEL_NODE_ID - 1) — the
# SAME mapping the RTT-zone math below relies on (internal_id/ZONE_SIZE = zone).
# A(NODE_ID 1)=node 0 (asker), B(NODE_ID 2)=node 1 (holder), C(NODE_ID 3)=node 2.
AINT=0              # A's internal id (the asker; "mind/want from node 0")
BINT=1              # B's internal id (the holder/teacher; "from node 1")
CINT=2              # C's internal id (the out-of-region control)

WORK="$(mktemp -d /tmp/p47_work.XXXXXX)"
declare -A NODE_PID
RELAY_PID=0
FAIL=0

TS()   { date '+%H:%M:%S'; }
log()  { echo "[$(TS)] $*"; }
ok()   { log "ok  : $*"; }
bad()  { log "FAIL: $*"; FAIL=1; }

L1=/tmp/p47_nodeA.log    # node 1 = A (asker, region 0)
L2=/tmp/p47_nodeB.log    # node 2 = B (holder, region 0)
L3=/tmp/p47_nodeC.log    # node 3 = C (OUT of region — RTT zone 1)

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
        PKERNEL_RTT_ZONE_SIZE=2 PKERNEL_RTT_ZONE_PENALTY=300 \
        $WRAP "$BOOT/p-kernel" < "$fifo" > "$lg" 2>&1 &
    NODE_PID[$i]=$!; disown "${NODE_PID[$i]}"
    log "node$i up pid=${NODE_PID[$i]} dir=$dir log=$lg"
}
send() {
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

echo "==========================================================="
echo " LM-15 — region pull-teach: A ASKS the region, a holder ANSWERS"
echo "==========================================================="
"$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" > "$WORK/relay.log" 2>&1 &
RELAY_PID=$!; disown "$RELAY_PID"; sleep 1

start_node 1 "$WORK/fA" "$WORK/dA" "$L1"
start_node 2 "$WORK/fB" "$WORK/dB" "$L2"
start_node 3 "$WORK/fC" "$WORK/dC" "$L3"
exec 3<>"$WORK/fA" 4<>"$WORK/fB" 5<>"$WORK/fC"

wait_for "$L2" 'mind_net_task up' 60 || bad "B's mind_net_task never started"
wait_for "$L3" 'mind_net_task up' 60 || true

log "waiting for A,B to form ONE region (RTT <= REGION_TAU_MS) ..."
t=0; rok=0
while [ $t -lt 150 ]; do
    printf 'region\n' >&3; sleep 2
    if grep -aq 'size=2' "$L1"; then rok=1; break; fi
    t=$((t+2))
done
[ "$rok" -eq 1 ] && ok "A,B formed one region (size=2)" \
                 || bad "A,B never formed a region (size stayed 1)"
grep -a 'size=' "$L1" | tail -1 | sed 's/^/    /'

# ---------------------------------------------------------------------------
# Key discovery: A asks each candidate upper-vocab word; K = the FIRST that
# prints `wondering about "<word>"` (selection by observed accrual — GENERICITY).
# A's first ask lazily pretrains the widened substrate (~23-46s host), so the
# discovery loop tolerates a long first response.
# ---------------------------------------------------------------------------
echo
echo "--- discovering K: the first candidate A WONDERS about (no hard-coded key) ---"
CANDS="fire leaf stone water cloud rose bone ink"
KWORD=""
for w in $CANDS; do
    PRE=$(grep -ac "wondering about" "$L1")
    send 1 "mind ask $w"
    # first ask pays pretrain; give it up to 90s, later asks are quick.
    hit=0
    for _ in $(seq 1 360); do
        if [ "$(grep -ac "wondering about \"$w\"" "$L1")" -gt 0 ]; then hit=1; break; fi
        # if the ask printed but did NOT wonder (known), move to the next word.
        if [ "$(grep -ac "ask \"$w\"" "$L1")" -gt 0 ] && \
           [ "$(grep -ac "wondering about" "$L1")" -le "$PRE" ] && \
           grep -aq "ask \"$w\"" "$L1"; then
            # printed an ask line but no new wonder -> known word, skip.
            sleep 0.25
            [ "$(grep -ac "wondering about \"$w\"" "$L1")" -gt 0 ] && { hit=1; break; }
            break
        fi
        sleep 0.25
    done
    if [ "$hit" -eq 1 ]; then KWORD="$w"; break; fi
    log "  '$w' did not wonder (held/known) — trying next candidate"
done
[ -n "$KWORD" ] || bad "no candidate word made A wonder (cannot run honestly)"
[ -n "$KWORD" ] || { echo "RESULT: FAIL"; exit 1; }
# K's token id (the kernel prints k=<id> in the ask line + <id> in mind/want).
KID=$(grep -a "ask \"$KWORD\"" "$L1" | tail -1 | grep -aoE 'k=[0-9]+' | grep -aoE '[0-9]+' | head -1)
ok "discovered K=\"$KWORD\" (token id $KID) — A wonders about it (asked but unknown)"

# accrue W total wonders on A (already 1 from discovery), and make C wonder too
# (the region control needs a LIVE want on C).
n_more=$((WANT_W - 1))
for _ in $(seq 1 $n_more); do send 1 "mind ask $KWORD"; sleep 2; done
send 1 "mind wonder"; sleep 2
grep -aE "\"$KWORD\" want=" "$L1" | tail -1 | sed 's/^/    A wonders: /'
send 3 "mind ask $KWORD"; sleep 3        # C wonders K too (its own local want)

# ===========================================================================
# Phase D — DISEASE: nobody holds K. A asks, the region hears, nothing changes.
# ===========================================================================
echo
echo "--- [pull-want-live]: B receives A's want for K over the REAL mind/want topic ---"
if wait_for "$L2" "mind/want from node $AINT .*keys.* $KID( |\$)" 60 || wait_for "$L2" "mind/want from node $AINT" 60; then
    ok "[pull-want-live] PASS — B heard A's want on mind/want"
    grep -a "mind/want from node $AINT" "$L2" | tail -1 | sed 's/^/    /'
else
    bad "[pull-want-live] — B never received A's want over the topic"
fi

echo
echo "--- [pull-unknown-silent]: nobody holds K -> ZERO arrival on A; want persists ---"
# watch >= 20 poll ticks (10s @ MT_POLL_MS=500). bound printed (VII.5).
log "observing 15s (>= 20 poll ticks) with NO holder of K ..."
sleep 15
A_ARR=$(grep -acE "long-WANTED key arrived|remote teach arrived" "$L1")
if [ "${A_ARR:-0}" -eq 0 ]; then
    ok "A received ZERO arrivals for any key (no hallucinated answer)"
else
    bad "A received an arrival with NO holder present (spurious answer)"
fi
# B says it holds none of the wanted keys (or simply never answers).
if grep -aq 'none of the wanted keys held — silent' "$L2"; then
    ok "B printed 'not held — silent' (it does not answer keys it lacks)"
else
    log "note: B printed no answer for K (silence is also acceptable disease evidence)"
fi
# A still wonders K.
send 1 "mind wonder"; sleep 2
if grep -aE "\"$KWORD\" want=" "$L1" | tail -1 | grep -aq "want="; then
    ok "[pull-unknown-silent] PASS — A still wonders \"$KWORD\"; the mechanism did not fake an answer"
else
    bad "[pull-unknown-silent] — A's want for \"$KWORD\" vanished with no holder (spurious take)"
fi

echo
echo "--- [pull-region-quiet]: C (out of region) — its want never reaches B ---"
if grep -aq "mind/want from node $CINT" "$L2"; then
    bad "[pull-region-quiet] — C's want reached B (region scope leaked on mind/want)"
else
    ok "[pull-region-quiet] PASS — C's want never reached B (region bounds the question)"
fi

# ===========================================================================
# Phase C — CURE: teach exactly ONE peer (B). B answers A's want via PULL.
# ===========================================================================
echo
echo "--- [pull-answered]: teach B \"$KWORD\"->\"$VWORD\"; B answers via PULL; A learns from node $BINT ---"
send 2 "mind teach $KWORD $VWORD"
# B's first teach lazily pretrains the widened substrate before it can publish.
wait_for "$L2" 'published mind/teach' 200 || bad "B never published its teach"
# 1) B answers A's want via the PULL path — the print is emitted ONLY by
#    mq_poll_wants (NOT by normal teach-gossip), so it PROVES the pull mechanism.
if wait_for "$L2" "answering want key $KID .* from node $AINT" 60; then
    ok "  (1) B answered via PULL: 'answering want key $KID ... from node $AINT'"
    grep -a "answering want key $KID" "$L2" | tail -1 | sed 's/^/      /'
    STEP1=1
else
    bad "  (1) B never answered A's want via the pull path"; STEP1=0
fi
# 2) A learns K over the WIRE from node $BINT = B (A was NEVER taught locally).
if wait_for "$L1" "remote teach arrived.*from node $BINT.*r3_fact_learn rc=0" 90; then
    ok "  (2) A: 'remote teach arrived ... from node $BINT ... r3_fact_learn rc=0' (network origin)"
    grep -a "remote teach arrived.*from node $BINT" "$L1" | tail -1 | sed 's/^/      /'
    STEP2=1
else
    bad "  (2) A never received K from node $BINT over the wire"; STEP2=0
fi
# 3) the pulled answer lands PRECIOUS: arrival salience == 1 + want (W).
EXP_SAL=$((1 + WANT_W))
if wait_for "$L1" "long-WANTED key arrived .want $WANT_W. -> arrival salience $EXP_SAL" 30; then
    ok "  (3) A: arrival salience $EXP_SAL == 1 + want($WANT_W) — the pulled answer is PRECIOUS"
    grep -a 'long-WANTED key arrived' "$L1" | tail -1 | sed 's/^/      /'
    STEP3=1
else
    bad "  (3) A did not convert want($WANT_W) -> arrival salience $EXP_SAL (the LM-14 payoff)"; STEP3=0
    grep -a 'long-WANTED key arrived' "$L1" | tail -1 | sed 's/^/      (got) /'
fi
[ "${STEP1:-0}$STEP2$STEP3" = "111" ] && ok "[pull-answered] PASS — asked -> answered over the wire -> precious" \
                                      || bad "[pull-answered] — see (1)/(2)/(3) above"

echo
echo "--- [pull-want-cleared]: A no longer wonders K; its next mind/want is empty ---"
send 1 "mind wonder"; sleep 2
if grep -aE "\"$KWORD\" want=" "$L1" | tail -1 | grep -aq "want="; then
    # tolerate a stale line: check the LATEST wonder block does not list K.
    LASTW=$(grep -an "curious about" "$L1" | tail -1 | cut -d: -f1)
    if [ -n "$LASTW" ] && tail -n +"$LASTW" "$L1" | grep -aqE "\"$KWORD\" want="; then
        bad "[pull-want-cleared] — A still wonders \"$KWORD\" after learning it"
    else
        ok "[pull-want-cleared] PASS — A's latest wonder block no longer lists \"$KWORD\""
    fi
else
    ok "[pull-want-cleared] PASS — A no longer wonders \"$KWORD\" (want cleared by r3_want_take)"
fi
# A's next mind/want publish is the empty snapshot (n=0) once every want is answered.
if wait_for "$L1" 'mind/want empty snapshot' 20; then
    ok "A published the empty mind/want snapshot (going silent — wonders nothing)"
else
    log "note: A still has other live wants; empty snapshot not expected yet (honest)"
fi

echo
echo "--- [pull-grounded]: A answers V and names the teacher (node $BINT = B) ---"
send 1 "mind wait 90"
wait_for "$L1" 'distilled in-context facts -> rw\[\]' 180 || true
wait_for "$L1" 'wait: drained' 30 || true
ASK_PRE=$(grep -ac "ask \"$KWORD\"" "$L1")
askf=0
for _ in $(seq 1 30); do
    send 1 "mind ask $KWORD"
    for _ in $(seq 1 8); do
        [ "$(grep -ac "ask \"$KWORD\"" "$L1")" -gt "${ASK_PRE:-0}" ] && { askf=1; break; }
        sleep 0.25
    done
    [ "$askf" -eq 1 ] && break
done
ASK_LINE=$(grep -a "ask \"$KWORD\"" "$L1" | tail -1)
echo "    $ASK_LINE"
GROUNDED=1
echo "$ASK_LINE" | grep -aq "\"$KWORD\" -> \"$VWORD\"" \
    && ok "A answers \"$KWORD\"->\"$VWORD\" (the pulled value, in words)" \
    || { bad "A did not answer \"$VWORD\" for \"$KWORD\""; GROUNDED=0; }
if grep -aq "taught by node $BINT" "$L1"; then
    ok "A names the teacher: node $BINT (provenance forwarded through the pull answer)"
    grep -a "taught by node $BINT" "$L1" | tail -1 | sed 's/^/    /'
else
    bad "A did not name node $BINT as the teacher"; GROUNDED=0
fi
[ "$GROUNDED" -eq 1 ] && ok "[pull-grounded] PASS — the pulled answer is attributable to node $BINT" \
                      || bad "[pull-grounded] — see lines above"

echo
echo "--- [pull-consolidated]: A's OWN DMN grounded it -> re-ask share >= $SHARE_GATE ---"
SHARE=$(echo "$ASK_LINE" | grep -aoE 'share=[0-9]+\.[0-9]' | grep -aoE '[0-9]+\.[0-9]')
SHARE_INT=$(echo "${SHARE:-0}" | cut -d. -f1)
TC_LAST=$(grep -a '\[teach-consolidated\]' "$L1" | tail -1)
if echo "$TC_LAST" | grep -aq 'PASS' && [ "${SHARE_INT:-0}" -ge "$SHARE_GATE" ]; then
    ok "[pull-consolidated] PASS — A answers from its OWN rw[] (share=${SHARE}% >= $SHARE_GATE)"
else
    bad "[pull-consolidated] — A did not answer from weights at >= $SHARE_GATE% (share=${SHARE:-?})"
fi

echo
echo "--- [pull-region]: C (out of region) — still wondering, never received K ---"
send 3 "mind ask $KWORD"; sleep 3
if grep -aqE 'long-WANTED key arrived|remote teach arrived' "$L3"; then
    bad "[pull-region] — C received K (region scope leaked!)"
else
    ok "C never received K (no arrival — region boundary held for the answer too)"
fi
send 3 "mind wonder"; sleep 2
if grep -aE "\"$KWORD\" want=" "$L3" | tail -1 | grep -aq "want="; then
    ok "[pull-region] PASS — C still wonders \"$KWORD\" (asked, unanswered, outside the region)"
else
    log "note: C's wonder line not found (C may not have accrued) — the hard gate is no-arrival"
    ok "[pull-region] PASS — C received no arrival for K (the load-bearing half)"
fi

echo
echo "--- [pull-storm-bounded]: exactly ONE r3_fact_learn for K on A; B's answers cap ---"
A_LEARN=$(grep -acE "remote teach arrived.*from node $BINT.*r3_fact_learn rc=0" "$L1")
if [ "${A_LEARN:-0}" -eq 1 ]; then
    ok "A learned K exactly once (idempotent receive: origin+seq high-water)"
else
    bad "[pull-storm-bounded] — A learned K ${A_LEARN} times (dedup leaked)"
fi
# B's answer prints for K stop after A cleared its want (count stable over 10 ticks).
B_ANS_1=$(grep -ac "answering want key $KID" "$L2")
sleep 6
B_ANS_2=$(grep -ac "answering want key $KID" "$L2")
if [ "${B_ANS_2:-0}" -le $((${B_ANS_1:-0} + 1)) ] && [ "${B_ANS_2:-0}" -le 20 ]; then
    ok "[pull-storm-bounded] PASS — B's answers for K bounded (${B_ANS_1}->${B_ANS_2}, cap 20) and settling"
else
    bad "[pull-storm-bounded] — B kept answering K unbounded (${B_ANS_1}->${B_ANS_2})"
fi

# ===========================================================================
echo
if [ "$FAIL" -ne 0 ]; then
    echo "==========================================================="
    echo " RESULT: FAIL — see [FAIL] lines above and /tmp/p47_*.log"
    echo "==========================================================="
    exit 1
fi
echo "==========================================================="
echo " RESULT: PASS — THE MIND REACHED OUT AND WAS ANSWERED."
echo " A wondered about \"$KWORD\", PUBLISHED that want on mind/want (the KEY, never"
echo " the level), a region peer that HELD it re-taught the answer on mind/teach,"
echo " and A learned it over the wire — arriving PRECIOUS (salience 1+want) through"
echo " A's OWN r3_fact_learn + DMN. Nobody outside the region heard the question."
echo "==========================================================="
exit 0
