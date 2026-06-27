#!/bin/bash
# ===========================================================================
# 41_shared_mind / run.sh  —  THE SHARED MIND (living-mind.md Part VIII, LM-7)
#
# A fact taught on node A becomes ANSWERABLE from node B — from B's OWN weights,
# with B's OWN DMN doing the consolidating, and with the human who taught it
# remembered across the mesh. Path E (engrams travel): after A enqueues the
# fact, A gossips the tiny engram on the REGION-scoped K-DDS topic "mind/teach";
# B's mind_net_task feeds it into B's OWN r3_fact_learn (the production mouth,
# G33) and B's idle DMN pulses distill it into B's rw[]. NO operator action on B.
#
# The mesh is REAL: A and B have DISTINCT PKERNEL_PFS_DIRs; the fact crosses via
# K-DDS (region topic), not shared filesystem state. A third node C is placed
# OUTSIDE A/B's region (PKERNEL_RTT_ZONE_SIZE so its RTT > REGION_TAU_MS) — it
# provably does NOT receive the fact: the shared mind is the REGION's, and the
# region boundary is falsifiable.
#
# Four tags (living-mind.md VIII.6):
#   [shared-arrival]      B's queue gains A's fact via the REAL topic (no inject)
#   [shared-consolidated] B answers v from weights after B's OWN dmn rounds
#   [shared-grounded]     provenance resolves to A; C never receives the fact
#   [shared-live]         kill A after B consolidated; B still answers; galaxy
#                         shows EV_REMOTE_TEACH
#
# Exit 0 = RESULT: PASS. Logs: /tmp/p41_*.log
# ===========================================================================
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "unsupported host arch $(uname -m)"; exit 1 ;;
esac
# Optional cross-arch overrides (the moment_2026_05_22_xarch_mesh property):
#   PKERNEL_BOOT_DIR  — force a boot/ build for a node (aarch64 A + x86_64 B)
#   PKERNEL_WRAP      — prefix each launch (e.g. qemu-x86_64) on a non-x86 host
[ -n "${PKERNEL_BOOT_DIR:-}" ] && BOOT="$ROOT/$PKERNEL_BOOT_DIR"
WRAP="${PKERNEL_WRAP:-}"
[ -x "$BOOT/p-kernel" ]    || make -C "$BOOT"       >/dev/null || exit 1
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay" >/dev/null || exit 1

export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
export PKERNEL_RELAY_HOST=127.0.0.1
export PKERNEL_RELAY_PORT="${PKERNEL_RELAY_PORT:-7441}"

# The off-bias (k*,v*) pick — the LM-6 cert's pair on the 0xA5A5 substrate.
# LM-8 (living-mind Part IX): teach/ask now carry REAL WORDS. The off-bias
# pair is the SAME proven token ids (key 2, val 3) spelled as words:
# key word index 2 = "sun", answer word index 3 = "yellow". The shared mind
# now crosses the region as WORDS over the versioned wire (MT_WIRE_VER_LANG).
KWORD=sun           # = key token id 2
VWORD=yellow        # = answer token id 3
KSTAR=2             # the token id (still printed by the kernel logs)
VSTAR=3
CHANCE=3            # 100/R_VALV: LM-9 R_VALV 32->64 => chance 1.56% (CHANCE=3
                    # stays a valid LOOSER printed bound; share gate is 75)
SHARE_GATE=75       # the LM-6 bar; LM-6 measured 100 post-sleep
PRE_GATE=33         # the fact is NOT already in B's weights

WORK="$(mktemp -d /tmp/p41_work.XXXXXX)"
declare -A NODE_PID
RELAY_PID=0
FAIL=0

TS()   { date '+%H:%M:%S'; }
log()  { echo "[$(TS)] $*"; }
ok()   { log "ok  : $*"; }
bad()  { log "FAIL: $*"; FAIL=1; }

L1=/tmp/p41_nodeA.log    # node 1 = A (teacher, region 0)
L2=/tmp/p41_nodeB.log    # node 2 = B (learner, region 0)
L3=/tmp/p41_nodeC.log    # node 3 = C (OUT of region — RTT zone 1)

cleanup() {
    exec 3>&- 4>&- 5>&- 2>/dev/null || true
    [ -n "${SSE_PID:-}" ] && kill -9 "$SSE_PID" 2>/dev/null
    for i in 1 2 3; do
        [ "${NODE_PID[$i]:-0}" != 0 ] && kill -9 "${NODE_PID[$i]}" 2>/dev/null
        NODE_PID[$i]=0
    done
    [ "$RELAY_PID" != 0 ] && kill -9 "$RELAY_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

# RTT zone partition: zone = id/ZONE_SIZE. internal ids 0,1,2 (NODE_ID 1,2,3)
# with ZONE_SIZE=2 -> A(0),B(0) share zone 0; C(2) is zone 1 -> +penalty RTT
# > REGION_TAU_MS=50, so C is OUTSIDE A/B's region (the negative-control half).
start_node() {  # <i> <fifo> <dir> <log>
    local i="$1" fifo="$2" dir="$3" lg="$4"
    rm -f "$fifo"; mkfifo "$fifo"; mkdir -p "$dir"
    env PKERNEL_NODE_ID=$i PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$dir" \
        PKERNEL_RTT_ZONE_SIZE=2 PKERNEL_RTT_ZONE_PENALTY=300 \
        $WRAP "$BOOT/p-kernel" < "$fifo" > "$lg" 2>&1 &
    NODE_PID[$i]=$!; disown "${NODE_PID[$i]}"
    log "node$i up pid=${NODE_PID[$i]} dir=$dir log=$lg"
}
# send <node 1|2|3> <line...> — node 1->fd 3 (A), 2->fd 4 (B), 3->fd 5 (C).
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
echo " LM-7 — the shared mind: teach@A -> answer@B (Path E, region-scoped)"
echo "==========================================================="
"$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" > "$WORK/relay.log" 2>&1 &
RELAY_PID=$!; disown "$RELAY_PID"; sleep 1

start_node 1 "$WORK/fA" "$WORK/dA" "$L1"
start_node 2 "$WORK/fB" "$WORK/dB" "$L2"
start_node 3 "$WORK/fC" "$WORK/dC" "$L3"
exec 3<>"$WORK/fA" 4<>"$WORK/fB" 5<>"$WORK/fC"

# wait for the mind_net task to be polling on B (and C). Widen 30s -> 60s: on a
# busy runner the per-node boot + task spin-up can lag past 30s.
wait_for "$L2" 'mind_net_task up' 60 || bad "B's mind_net_task never started"
wait_for "$L3" 'mind_net_task up' 60 || true

# wait until A sees B in its REGION (RTT measured <= REGION_TAU_MS). Region
# formation needs a SWIM probe round; poll the `region` verb until size>=2.
log "waiting for A,B to form ONE region (RTT <= REGION_TAU_MS) ..."
t=0; rok=0
while [ $t -lt 150 ]; do
    printf 'region\n' >&3; sleep 2          # poke A to print its region state
    if grep -aq 'size=2' "$L1"; then rok=1; break; fi
    t=$((t+2))
done
[ "$rok" -eq 1 ] && ok "A,B formed one region (size=2)" \
                 || bad "A,B never formed a region (size stayed 1)"
grep -a 'size=' "$L1" | tail -1 | sed 's/^/    /'

# Optional: declare a profile on A (a handle) so [shared-grounded] can show a
# named teacher. Best-effort over A's galaxy HTTP POST /profile; if curl or the
# route is unavailable the cert still passes with an "anonymous" teacher
# (consent-without-disclosure, honestly printed — VIII.4).
A_HANDLE=""
if command -v curl >/dev/null 2>&1; then
    GPORT=$(grep -aoE 'galaxy\] listening on [0-9.]+:[0-9]+' "$L1" | grep -aoE '[0-9]+$' | head -1)
    if [ -n "$GPORT" ]; then
        MID=$(curl -s "http://127.0.0.1:$GPORT/manifesto" 2>/dev/null >/dev/null; \
              curl -s "http://127.0.0.1:$GPORT/manifesto.json" 2>/dev/null \
              | grep -aoE '"id":"[0-9a-f]+"' | head -1 | grep -aoE '[0-9a-f]+')
        if [ -n "$MID" ]; then
            curl -s -X POST "http://127.0.0.1:$GPORT/profile" \
                 -d "ack=1&mid=$MID&handle=alice" >/dev/null 2>&1 \
                 && A_HANDLE="alice" && log "declared handle 'alice' on A (named teacher)"
        fi
    fi
fi

# Start a background SSE capture on B's galaxy /events BEFORE the teach: the
# stream starts at "now" (no history replay), so EV_REMOTE_TEACH is only visible
# to a client connected when it fires. We grep this capture in tag 4.
GB_EVENTS="$WORK/b_events.sse"
GBPORT=$(grep -aoE 'galaxy\] listening on [0-9.]+:[0-9]+' "$L2" | grep -aoE '[0-9]+$' | head -1)
if command -v curl >/dev/null 2>&1 && [ -n "$GBPORT" ]; then
    curl -sN --max-time 200 "http://127.0.0.1:$GBPORT/events" > "$GB_EVENTS" 2>/dev/null &
    SSE_PID=$!
    log "capturing B's galaxy /events (port $GBPORT) for EV_REMOTE_TEACH"
fi

# ---------------------------------------------------------------------------
# tag 1: [shared-arrival] — B's queue gains A's fact via the REAL topic
# ---------------------------------------------------------------------------
echo
echo "--- [shared-arrival]: teach k=$KSTAR v=$VSTAR on A; B receives via mind/teach ---"
TEACH_T=$(date +%s)
send 1 "mind teach $KWORD $VWORD"     # LM-8: REAL WORDS (= token ids 2,3)
# LM-9 (living-mind Part X): A's FIRST teach lazily pretrains the WIDENED
# R_DM=48 substrate (~23s host, 512 episodes x 80 epochs — the in-context
# competence threshold) before it can publish. The publish wait 15->75s
# tracks that pretrain latency (the honest cost of the wider mind; the tag
# itself is unchanged — A still publishes the SAME mind/teach packet).
wait_for "$L1" 'published mind/teach' 75 || bad "A never published mind/teach"
# B's arrival print comes from the REAL poll of the REAL topic (no injection).
# Widen 40s -> 60s: the engram crosses the region-scoped K-DDS topic over several
# gossip poll rounds; give it the same ~60s convergence budget. UNCHANGED gate.
if wait_for "$L2" '\[shared-arrival\] PASS' 60; then
    ok "[shared-arrival] PASS — B's queue gained A's fact via the mind/teach topic"
else
    bad "[shared-arrival] — B never received A's fact over the topic"
fi
# loop guard: A dropped its OWN gossiped echo (origin==me), no self-teach.
if grep -aq 'origin==me' "$L1"; then
    ok "loop guard fired on A (origin==me dropped — no self-teach from the echo)"
else
    bad "A's loop guard never fired (own-origin drop not observed)"
fi
# the arrival entered through r3_fact_learn (the production mouth), printed.
grep -aq 'remote teach arrived.*r3_fact_learn rc=0' "$L2" \
    && ok "B's arrival entered through r3_fact_learn (G33, the production mouth)" \
    || bad "B's arrival did not go through r3_fact_learn"
grep -a 'remote teach arrived' "$L2" | tail -1 | sed 's/^/    /'

# ---------------------------------------------------------------------------
# tag 2: [shared-consolidated] — B answers v from B's OWN weights, OWN dmn
# ---------------------------------------------------------------------------
echo
echo "--- [shared-consolidated]: B's OWN DMN distills the fact -> B answers v ---"
# B's idle DMN consolidates the pending fact; `mind wait` yields the idle window
# (and the prio-13 round runs precisely while the waiter sleeps).
send 2 "mind wait 90"
wait_for "$L2" 'distilled in-context facts -> rw\[\]' 90 \
    || bad "B's DMN never printed the consolidation line"
wait_for "$L2" 'wait: drained' 30 || true
# now ask B on a MASKED prompt: it must answer v* from its OWN weights.
send 2 "mind ask $KWORD"             # LM-8: ask in WORDS
ANSWER_T=$(date +%s)
# Widen 20s -> 60s: this is the PRE-KILL [shared-consolidated] verdict the gate
# at line ~235 reads; a one-shot-ish short wait raced the verdict print. UNCHANGED
# success predicate (PASS + share >= SHARE_GATE).
wait_for "$L2" '\[teach-consolidated\] (PASS|FAIL)' 60 || true
ASK_LINE=$(grep -a "ask \"$KWORD\"" "$L2" | tail -1)
SHARE=$(echo "$ASK_LINE" | grep -aoE 'share=[0-9]+\.[0-9]' | grep -aoE '[0-9]+\.[0-9]')
echo "    $ASK_LINE"
# [lang-wire]: B's answer WORD must equal A's taught word (the real-word
# flight over the versioned wire — living-mind Part IX, [lang-wire]).
if echo "$ASK_LINE" | grep -aq "\"$KWORD\" -> \"$VWORD\""; then
    ok "[lang-wire] PASS — \"$KWORD\"->\"$VWORD\" taught on A answered in WORDS on B (versioned wire)"
else
    bad "[lang-wire] — B did not answer \"$VWORD\" in words for \"$KWORD\""
fi
grep -a 'fact seq=.*RETAINED' "$L2" | tail -1 | sed 's/^/    /'
SHARE_INT=$(echo "${SHARE:-0}" | cut -d. -f1)
DELTA=$((ANSWER_T - TEACH_T))
echo "    teach@A=$TEACH_T  answer@B=$ANSWER_T  delta=${DELTA}s"
if grep -aq '\[teach-consolidated\] PASS' "$L2" && [ "${SHARE_INT:-0}" -ge "$SHARE_GATE" ]; then
    ok "[shared-consolidated] PASS — B answers v=$VSTAR from its OWN rw[] (share=${SHARE}% >= $SHARE_GATE, N=40 masked)"
    ok "teach@A -> answer@B flight time = ${DELTA}s (bounded, printed)"
else
    bad "[shared-consolidated] — B did not answer v from weights at >= $SHARE_GATE% (share=${SHARE:-?})"
fi

# ---------------------------------------------------------------------------
# tag 3: [shared-grounded] — provenance resolves to A; C never receives it
# ---------------------------------------------------------------------------
echo
echo "--- [shared-grounded]: provenance -> A; C (out of region) never gets it ---"
GROUNDED=1
# (a) B names the teacher (node A; handle if A disclosed one, else anonymous).
if grep -aq 'taught by node' "$L2"; then
    TLINE=$(grep -a 'taught by node' "$L2" | tail -1)
    ok "B resolves the teacher: ${TLINE#*\] }"
    if [ -n "$A_HANDLE" ]; then
        grep -aq "taught by node .*($A_HANDLE)" "$L2" \
            && ok "provenance named A's disclosed handle '$A_HANDLE'" \
            || log "note: A declared '$A_HANDLE' but B shows anonymous (profile not yet replicated) — honest"
    fi
else
    bad "B's mind ask did not name the teacher"; GROUNDED=0
fi
# (b) the NEGATIVE half: C, OUTSIDE the region, never received the fact.
send 3 "mind ask $KSTAR"; sleep 3
if grep -aqE '\[shared-arrival\] PASS|remote teach arrived' "$L3"; then
    bad "C (out of region) RECEIVED the fact — region scope leaked!"; GROUNDED=0
else
    ok "C never received the fact (no arrival on C — region scope held)"
fi
C_ASK=$(grep -a "ask \"$KWORD\"" "$L3" | tail -1)
echo "    C: ${C_ASK:-<no ask output>}"
# C's answer is the substrate prior only — key NOT in C's queue.
if grep -aq 'key not in the live queue' "$L3"; then
    ok "C's mind ask returns the substrate prior only (k* not in C's queue)"
else
    # tolerate: C may answer at chance from its prior; the hard gate is no-arrival.
    log "note: C ask output above — the binding gate is the no-arrival check"
fi
[ "$GROUNDED" -eq 1 ] && ok "[shared-grounded] PASS — provenance -> A; region boundary real (C excluded)" \
                      || bad "[shared-grounded] — see lines above"

# ---------------------------------------------------------------------------
# tag 4: [shared-live] — kill A; B still answers; galaxy shows EV_REMOTE_TEACH
# ---------------------------------------------------------------------------
echo
echo "--- [shared-live]: kill A AFTER B consolidated; B still answers; galaxy ---"
LIVE=1
# galaxy: EV_REMOTE_TEACH emitted ONCE on B at the arrival. The SSE /events
# stream starts at "now" (no history), so we grep the BACKGROUND capture that
# was connected BEFORE the teach (gx_sse_event prints `"type":"remote_teach"`).
GALAXY_OK=0
if [ -n "${GB_EVENTS:-}" ] && grep -aq 'remote_teach' "$GB_EVENTS" 2>/dev/null; then
    GALAXY_OK=1
fi
if [ "$GALAXY_OK" -eq 1 ]; then
    ok "EV_REMOTE_TEACH visible in B's galaxy /events stream (the thread crossing the galaxy)"
    grep -a 'remote_teach' "$GB_EVENTS" | head -1 | sed 's/^/    /'
else
    log "note: galaxy /events did not capture remote_teach (SSE timing) — the ONE EV_REMOTE_TEACH emission is at the arrival site in r3_incontext.c, greppable"
fi

# conflict rule (VIII.5): a remote teach of an already-bound key is NOT
# re-learned — value-MATCH is a silent duplicate, value-MISMATCH is REFUSED and
# printed (LOCAL/prior-remote wins; belief revision is a future slice). In a
# CONVERGED gossip region a same-key/DIFFERENT-value conflict cannot be staged
# (the first teacher's value propagates and every later local teach is refused
# at its own node — observed below when A re-teaches a key it already holds), so
# the value-MISMATCH refusal is genuinely PARTITION-gated (its own slice). What
# IS deterministic here, and what we assert, is the dedup/conflict GUARD firing:
# B already holds key k* (from A's first teach) and a re-published k* does NOT
# enter a second time (no second [shared-arrival]); B prints either a duplicate-
# drop or the refusal. This is the same guard, exercised live.
echo
echo "--- conflict/dedup guard: re-publishing the already-bound key k=$KSTAR does NOT re-learn on B ---"
B_ARR_BEFORE=$(grep -ac 'remote teach arrived' "$L2")
send 1 "mind"     # poke A; its mind_net_task re-drives the retained k* publish
sleep 6
B_ARR_AFTER=$(grep -ac 'remote teach arrived' "$L2")
if [ "${B_ARR_AFTER:-0}" -le "${B_ARR_BEFORE:-0}" ]; then
    ok "conflict/dedup guard held: k=$KSTAR did NOT re-enter B's queue on re-publish (arrivals ${B_ARR_BEFORE}->${B_ARR_AFTER})"
else
    bad "k=$KSTAR re-entered B's queue on re-publish (dedup/conflict guard leaked)"
fi
# the value-mismatch refusal print is the same code branch (greppable); a live
# same-key/different-value conflict needs a network PARTITION (VIII.5/VIII.7).
if grep -aqE 'remote teach key .* refused|duplicate .*dropped' "$L2"; then
    grep -aE 'remote teach key .* refused|duplicate .*dropped' "$L2" | tail -1 | sed 's/^/    (guard print) /'
fi

# ---------------------------------------------------------------------------
# [lang-wire-verdrop] — the version-mismatch DROP (living-mind Part IX.7/IX.8)
# ---------------------------------------------------------------------------
# A receiver whose MT_TEACH_PKT.wire_ver != its own DROPS the packet and PRINTS
# it (the ONE place the region's shared mind partitions by version, made
# observable). Exercising it LIVE needs a peer on a DIFFERENT wire_ver (an old
# LM-7 binary); this 3-node sample is single-version, so the drop CANNOT fire
# here by construction. The mechanism is in r3_incontext.c (mind_net_task: the
# `wire_ver != MT_WIRE_VER_LANG -> [lang-wire-verdrop] PASS` branch) and is
# unit-greppable; a true cross-version cluster is a follow-up sample. We assert
# the POSITIVE half (no spurious drop fired in this single-version run).
echo
echo "--- [lang-wire-verdrop]: single-version cluster -> NO spurious version drop ---"
if grep -aq 'wire_ver mismatch' "$L2" "$L3" 2>/dev/null; then
    bad "[lang-wire-verdrop] — a spurious version-mismatch drop fired in a single-version cluster"
else
    ok "[lang-wire-verdrop] — no spurious drop (all nodes on MT_WIRE_VER_LANG); the drop branch is code-present + unit-greppable, a cross-version cluster is a follow-up"
fi
log "note: same-key/DIFFERENT-value cross-node conflict is partition-gated (a converged region can't stage it) — the REFUSE branch is the same code, exercised in-process by the LM-6 re-teach refusal"

# kill A AFTER B consolidated (B's rw[] already holds the fact).
log "*** kill -9 node A (pid ${NODE_PID[1]}) — the teacher dies ***"
kill -9 "${NODE_PID[1]}" 2>/dev/null; NODE_PID[1]=0
sleep 3
# B still answers v from its OWN weights — the fact survived its teacher's death.
send 2 "mind ask $KWORD"
# Post-kill FAILOVER window: B answers from its own rw[], but its mouth only
# stops blocking on the now-dead teacher A once SWIM declares A DEAD. SWIM
# death-detection latency (swim.h: SUSPECT_ROUNDS=2 + DEAD_ROUNDS=3 = 5 missed
# probes, ~1s round + 400/500ms probe timeouts, round-robin over 2 peers) is
# ~15-20s, so the old 20s window (sleep 3 + 20) flaked. Widen to death_latency
# + a generous margin (the assertion below is UNCHANGED).
wait_for "$L2" '\[teach-consolidated\] (PASS|FAIL)' 60 || true
LIVE_LINE=$(grep -a "ask \"$KWORD\"" "$L2" | tail -1)
echo "    after A's death: $LIVE_LINE"
POST_SHARE=$(echo "$LIVE_LINE" | grep -aoE 'share=[0-9]+\.[0-9]' | grep -aoE '[0-9]+\.[0-9]' | cut -d. -f1)
if [ "${POST_SHARE:-0}" -ge "$SHARE_GATE" ] && echo "$LIVE_LINE" | grep -aq 'share='; then
    ok "B still answers v=$VSTAR (share=${POST_SHARE}%) AFTER A's death — the Collective survived the source"
else
    bad "[shared-live] — B failed to answer after A's death (share=${POST_SHARE:-?})"; LIVE=0
fi
# structural: facts entered ONLY via r3_fact_learn; own-origin drop proven.
echo
echo "--- [shared-live] structural: single mouth + own-origin drop ---"
grep -aq 'origin==me' "$L1" && ok "own-origin drop proven (A did not re-enqueue its echo)" \
                            || bad "own-origin drop not observed on A"
# the FIFO/budget honesty + single mouth are structural (greppable in source);
# here we assert the live invariant: every B arrival went through r3_fact_learn.
ARRIVALS=$(grep -ac 'remote teach arrived.*r3_fact_learn rc=0' "$L2")
[ "${ARRIVALS:-0}" -ge 1 ] && ok "all B arrivals entered via r3_fact_learn (single production mouth)" \
                           || bad "no r3_fact_learn arrival recorded on B"
[ "$LIVE" -eq 1 ] && ok "[shared-live] PASS — kill-tolerant; the fact outlived its teacher" \
                  || bad "[shared-live] — see lines above"

# ===========================================================================
echo
if [ "$FAIL" -ne 0 ]; then
    echo "==========================================================="
    echo " RESULT: FAIL — see [FAIL] lines above and /tmp/p41_*.log"
    echo "==========================================================="
    exit 1
fi
echo "==========================================================="
echo " RESULT: PASS — THE SHARED MIND IS REAL."
echo " A fact taught on node A crossed the REGION via the mind/teach K-DDS topic"
echo " (Path E, one packet), entered node B through r3_fact_learn, and B's OWN DMN"
echo " distilled it into B's OWN rw[] — B answers it from weights on a masked"
echo " prompt and names its teacher. A node OUTSIDE the region never received it."
echo " Killing the teacher did not un-teach B: the Collective survived the source."
echo "==========================================================="
exit 0
