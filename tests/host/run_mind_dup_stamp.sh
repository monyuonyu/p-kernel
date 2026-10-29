#!/bin/bash
# tests/host/run_mind_dup_stamp.sh — [mind-dup-stamp] (ROADMAP 1-3m).
#
# The TLA+ model of the 1-3b rule (tests/model/mind_rule/, counterexample
# cex/S3-now.txt) found three nodes that never agree, with no packet lost:
# a node that already holds a value drops the same value from a better
# teacher as a "duplicate" and keeps its own stamp, while the sender's seq is
# used up; a later value with a stamp between the two then wins on that node
# alone, and every re-send of the better one is deduplicated.
#
# The same arrival order on ONE node (built with -DMT_RX_INJECT, the test hook
# `mind inject <node> <stamp> <key> <value>`): node 2 (PKERNEL_NODE_ID=3)
# teaches sky blue (stamp s, node 2), then gets
#   blue  from node 0 with stamp s   (same value, smaller node = better)
#   green from node 1 with stamp s
# The rule (larger stamp, then the smaller node) puts blue (s, node 0) above
# green (s, node 1) above blue (s, node 2): blue is the answer.
#   D0 the hook works: both packets taken, the first one seen as the same value
#   D1 green is refused against held stamp s node 0
#   D2 green does not replace blue
# and with a REMOTE engram held (audit-98b; D0-D2 pass without these parts):
#   D3 the better teacher is taken too (same stamp, a smaller node)
#   D4 the better stamp replaces the remote engram's stamp (mc_rstamp_put)
#   D5 the same value from a worse teacher is still dropped as a duplicate
# Env: MDS_PORT (default 7496), EXTRA_CFLAGS (added to -DMT_RX_INJECT, for a
# negative control). Output "[mind-dup-stamp] <check> PASS|FAIL", then
# ALL PASS (exit 0) or RED (exit 1).
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    *)             BOOT="$ROOT/boot/linux_x86_64" ;;
esac
PORT="${MDS_PORT:-7496}"
WORK="$(mktemp -d /tmp/mds.XXXXXX)"
REDS=""; RELAY=0; PC=0
res() { echo "[mind-dup-stamp] $1 $2"; [ "$2" = FAIL ] && REDS="$REDS $1"; return 0; }
cleanup() {
    for p in "$PC" "$RELAY"; do [ "$p" != 0 ] && kill -9 "$p" 2>/dev/null; done
    exec 7>&- 2>/dev/null; wait 2>/dev/null
    make -C "$BOOT" clean >/dev/null 2>&1
    rm -rf "$WORK"
}
trap cleanup EXIT
wait_for() {  # <file> <ERE> <secs>
    local n=0
    while [ "$n" -lt "$(( $3 * 2 ))" ]; do
        grep -aqE "$2" "$1" 2>/dev/null && return 0
        sleep 0.5; n=$((n + 1))
    done
    return 1
}

make -C "$ROOT/relay" >/dev/null || exit 1
make -C "$BOOT" clean >/dev/null 2>&1
make -C "$BOOT" EXTRA_CFLAGS="-DMT_RX_INJECT ${EXTRA_CFLAGS:-}" >/dev/null 2>&1 \
    || { echo "[mind-dup-stamp] BUILD FAILED"; exit 1; }

export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
export PKERNEL_GALAXY=0
unset PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN
"$ROOT/relay/relay" -p "$PORT" > "$WORK/relay.log" 2>&1 &
RELAY=$!
sleep 1
L="$WORK/c.log"; mkfifo "$WORK/fc"; mkdir -p "$WORK/dc"
env PKERNEL_NODE_ID=3 PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$WORK/dc" PKERNEL_RELAY="127.0.0.1:$PORT" \
    "$BOOT/p-kernel" < "$WORK/fc" > "$L" 2>&1 &
PC=$!; exec 7<>"$WORK/fc"
wait_for "$L" 'mind_net_task up' 90 || echo "[mind-dup-stamp] note: mind_net_task not seen"

printf 'mind teach sky blue\n' >&7
wait_for "$L" 'published mind/teach k=[0-9]+ v=[0-9]+ seq=[0-9]+ ' 60 \
    || echo "[mind-dup-stamp] note: blue not published"
S="$(grep -aoE 'published mind/teach k=[0-9]+ v=[0-9]+ seq=[0-9]+ ' "$L" | head -1 | sed -E 's/.*seq=([0-9]+) /\1/')"
[ -n "$S" ] || S=0
echo "[mind-dup-stamp] blue taught here with stamp $S"

printf 'mind inject 0 %s sky blue\n' "$S" >&7
wait_for "$L" 'duplicate \(already bound to same v\)|same value, took its stamp' 30 || echo "[mind-dup-stamp] note: first packet not seen"
printf 'mind inject 1 %s sky green\n' "$S" >&7
wait_for "$L" "conflict rule: incoming stamp $S node 1 " 30 || echo "[mind-dup-stamp] note: green not compared"
sleep 1

if [ "$(grep -ac 'inject: taken' "$L")" -eq 2 ] \
   && grep -aqE 'duplicate \(already bound to same v\)|same value, took its stamp' "$L"; then
    res D0-hook PASS; else res D0-hook FAIL; fi
if grep -aqE "conflict rule: incoming stamp $S node 1 loses to held stamp $S node 0" "$L"; then
    res D1-green-refused PASS; else res D1-green-refused FAIL; fi
if grep -aqE 'remote REVISE key [0-9]+ "sky": "blue"->"green"' "$L"; then
    res D2-blue-kept FAIL; else res D2-blue-kept PASS; fi

# audit-98b: the held engram is a remote teach (D0-D2 only see this node's
# own teach, which has no stamp or teacher entry yet). Stamps above S, a new
# seq per sender each time (the once-per-seq check), one packet at a time.
# D4 runs first: the stamp table has R3_FQ_MAX (4) slots, and once it is full
# a new entry overwrites slot 0, which hides a stamp that was not replaced.
NI=2
inj() {  # <node> <stamp> <key> <value> <ERE of the line it should cause>
    NI=$((NI + 1))
    printf 'mind inject %s %s %s %s\n' "$1" "$2" "$3" "$4" >&7
    local n=0
    while [ "$(grep -ac 'inject: taken' "$L")" -lt "$NI" ] && [ "$n" -lt 60 ]; do sleep 0.5; n=$((n + 1)); done
    wait_for "$L" "$5" 15 || echo "[mind-dup-stamp] note: no line for $*"
}
T=$((S + 10)); U=$((S + 20)); M=$((S + 25)); V=$((S + 30))
#   D4 the stamp of a remote engram (mc_rstamp_put on a seq it already has):
#      fire warm from node 3 at U, node 0 sends warm at V (taken), then blue
#      from node 1 at M (U < M < V) must lose to (V, node 0)
inj 3 "$U" fire warm "remote teach arrived: \"fire\""
inj 0 "$V" fire warm "took its stamp $V \\(was stamp $U node 3\\)"
inj 1 "$M" fire blue "conflict rule: incoming stamp $M node 1 "
if grep -aqE "conflict rule: incoming stamp $M node 1 loses to held stamp $V node 0" "$L"; then
    res D4-stamp-taken PASS; else res D4-stamp-taken FAIL; fi
#   D3 the teacher too (same stamp): sea deep learnt from node 3 at T, node 0
#      sends deep at T (smaller node: taken), then green from node 1 at T
#      must lose to (T, node 0) — it beats (T, node 3) if only the stamp moved
#      (by now the 4-slot prov table is full, so a teacher that is not
#      re-pointed shows in D4, not here; D3 is the equal-stamp case)
inj 3 "$T" sea deep  "remote teach arrived: \"sea\""
inj 0 "$T" sea deep  "took its stamp $T \\(was stamp $T node 3\\)"
inj 1 "$T" sea green "conflict rule: incoming stamp $T node 1 "
if grep -aqE "conflict rule: incoming stamp $T node 1 loses to held stamp $T node 0" "$L"; then
    res D3-teacher-taken PASS; else res D3-teacher-taken FAIL; fi
#   D5 a worse teacher of the same value is not taken: sea deep from node 4
#      with the same stamp T against (T, node 0)
inj 4 "$T" sea deep  "from node 4 — duplicate|took its stamp $T \\(was stamp $T node 0\\)"
if grep -aqE "from node 4 — duplicate \(already bound to same v\)" "$L" \
   && ! grep -aqE "took its stamp $T \(was stamp $T node 0\)" "$L"; then
    res D5-worse-dropped PASS; else res D5-worse-dropped FAIL; fi
grep -aE 'inject:|duplicate|conflict rule:|remote REVISE|took its stamp' "$L" | sed 's/^/  /'

if [ -z "$REDS" ]; then echo "[mind-dup-stamp] ALL PASS"; exit 0; fi
echo "[mind-dup-stamp] RED:$REDS"; exit 1
