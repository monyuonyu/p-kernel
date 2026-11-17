#!/bin/bash
# ---------------------------------------------------------------------------
# run_b5_wire_live.sh - B5 wire, stage 2 live (research/b5-wire2.md §3 live):
# the resident student's periodic round with its region, relay + 5 hosted
# nodes, PKERNEL_B5_ROUND_S = the round. Lines fixed in the design before the
# code:
#   L1  every node has an automatic line for two consecutive generations G1,
#       G1+1, with members 5 and fetched 4 (the same gen on every node: the
#       generation is the wall clock / the round)
#   L2  from the head of G1 to the end of G1+1: no "-> SUSPECT", no
#       SELF-SUSPICION, no "want table full" on any node, and at most 64
#       "[pfs] announce" lines per node per round
#   L3  node 5 `stop now` after G1+1's merge: no automatic line of G1+2 on
#       node 5; the others' G1+2 line has same-gen 4
#   L4  in the middle of G1+1's merge on node 1: `baby region 0` says busy and
#       `ver` answers, each within 2 s
# Arms: B5WL_TIER=S (default; 120 s rounds, the CI candidate) or M (900 s,
# by hand). Arg: plain (default) | nc-loud | nc-stop | nc-lock - the falsifier
# builds -DB5W_NC_LOUD (L2 red), -DB5W_NC_NO_STOP (L3 red), -DB5W_NC_NO_LOCK
# (L4 red); an nc arm exits 0 when exactly its line is red.
# Rebuilds boot/linux* with EXTRA_CFLAGS and cleans it afterwards.
# Env: B5WL_PORT (7591), B5WL_TIER, B5WL_ROUND (s), B5WL_KEEP=<dir> keeps logs.
# Output: "[b5w-live] <line> PASS|FAIL ...", then ALL PASS (0) or RED (1).
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    *)             BOOT="$ROOT/boot/linux_x86_64" ;;
esac
MODE="${1:-plain}"
TIER="${B5WL_TIER:-S}"
if [ "$TIER" = M ]; then P="${B5WL_ROUND:-900}"; else P="${B5WL_ROUND:-120}"; fi
N=5
CF=""; WANT_RED=""
case "$MODE" in
    plain)   ;;
    nc-loud) CF="-DB5W_NC_LOUD";    WANT_RED="L2" ;;
    nc-stop) CF="-DB5W_NC_NO_STOP"; WANT_RED="L3" ;;
    nc-lock) CF="-DB5W_NC_NO_LOCK"; WANT_RED="L4" ;;
    *) echo "usage: $0 [plain|nc-loud|nc-stop|nc-lock]"; exit 2 ;;
esac
CC="${CC:-cc}"
make -C "$BOOT" clean >/dev/null 2>&1
make -C "$BOOT" EXTRA_CFLAGS="$CF" > /tmp/b5wl.build.log 2>&1 \
    || { echo "[b5w-live] build FAIL"; tail -20 /tmp/b5wl.build.log; exit 1; }
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay" >/dev/null || exit 1
export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
unset PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN
PORT="${B5WL_PORT:-7591}"
WORK="$(mktemp -d /tmp/b5wl.XXXXXX)"
REDS=""; RELAY=0; PIDS=""
res() { echo "[b5w-live] $1 $2 $3"; [ "$2" = FAIL ] && REDS="$REDS $1"; return 0; }
cleanup() {
    for p in $PIDS $RELAY; do [ "$p" != 0 ] && kill -9 "$p" 2>/dev/null; done
    for k in 1 2 3 4 5; do eval "exec $((10 + k))>&-" 2>/dev/null; done
    wait 2>/dev/null
    if [ -n "${B5WL_KEEP:-}" ]; then mkdir -p "$B5WL_KEEP"; cp "$WORK"/*.log "$B5WL_KEEP"/ 2>/dev/null; fi
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
"$ROOT/relay/relay" -p "$PORT" > "$WORK/relay.log" 2>&1 &
RELAY=$!
sleep 1
for k in 1 2 3 4 5; do
    mkfifo "$WORK/f$k"; mkdir -p "$WORK/d$k"
    env PKERNEL_NODE_ID=$k PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$WORK/d$k" PKERNEL_RELAY="127.0.0.1:$PORT" \
        PKERNEL_DEVICE_TIER="$TIER" PKERNEL_B5_ROUND_S="$P" \
        "$BOOT/p-kernel" < "$WORK/f$k" > "$WORK/n$k.log" 2>&1 &
    PIDS="$PIDS $!"
    eval "exec $((10 + k))<>\"$WORK/f$k\""
done
say() { printf '%s\n' "$2" >&$((10 + $1)); }   # say <node> <line>
for k in 1 2 3 4 5; do
    wait_for "$WORK/n$k.log" 'student_region_task up' 180 || { res L0 FAIL "node $k: no student_region_task"; }
done
echo "[b5w-live] tier $TIER round $P s, mode $MODE, nodes up at $(date +%s)"
# G1: the first generation whose head is at least 10 s ahead (every node sees its head)
now=$(date +%s)
G1=$(( (now + 10 + P - 1) / P ))
H1=$(( G1 * P ))
echo "[b5w-live] G1 $G1 head in $(( H1 - now )) s"
sleep_until() { local t; t=$(( $1 - $(date +%s) )); [ "$t" -gt 0 ] && sleep "$t"; return 0; }
mark() { for k in 1 2 3 4 5; do wc -l < "$WORK/n$k.log"; done | tr '\n' ' '; }
sleep_until $(( H1 - 2 ))
M0="$(mark)"
# L4: in the middle of G1+1's merge on node 1
G2=$(( G1 + 1 ))
sleep_until $(( G2 * P + P / 2 + 3 ))
L4a=$(wc -l < "$WORK/n1.log")
t0=$(date +%s%N)
say 1 "baby region 0"
say 1 "ver"
busy=0; ver=0; tb=0; tv=0
for i in $(seq 1 40); do
    sleep 0.1
    if [ "$busy" = 0 ] && tail -n +"$L4a" "$WORK/n1.log" | grep -aq '\[baby\] region: busy'; then busy=1; tb=$(( ($(date +%s%N) - t0) / 1000000 )); fi
    if [ "$ver" = 0 ] && tail -n +"$L4a" "$WORK/n1.log" | grep -aq 'User-Mode p-kernel'; then ver=1; tv=$(( ($(date +%s%N) - t0) / 1000000 )); fi
    [ "$busy" = 1 ] && [ "$ver" = 1 ] && break
done
# was node 1 really mid-merge? (its G1+1 line comes after the probe)
for k in 1 2 3 4 5; do wait_for "$WORK/n$k.log" "\[baby\] auto: gen $G2 " $(( P / 2 )) || true; done
M1="$(mark)"
after=$(tail -n +"$L4a" "$WORK/n1.log" | grep -ac "\[baby\] auto: gen $G2 ")
if [ "$busy" = 1 ] && [ "$ver" = 1 ] && [ "$after" -ge 1 ]; then res L4 PASS "busy ${tb} ms, ver ${tv} ms, the merge line after the probe"
else res L4 FAIL "busy $busy (${tb} ms) ver $ver (${tv} ms) merge-after $after"; fi
# L1
l1=1; l1why=""
for k in 1 2 3 4 5; do
    for g in $G1 $G2; do
        ln=$(grep -a "\[baby\] auto: gen $g " "$WORK/n$k.log" | head -1)
        echo "  n$k: ${ln:-<none for gen $g>}"
        echo "$ln" | grep -q "members 5 fetched 4 " || { l1=0; l1why="$l1why n$k/g$g"; }
    done
done
if [ "$l1" = 1 ]; then res L1 PASS "5 nodes x gens $G1 $G2: members 5 fetched 4"; else res L1 FAIL "missing or short:$l1why"; fi
# L2: the window [head of G1 - 2 s, the last G1+1 line]
l2=1; l2why=""
set -- $M0; a0=("$@"); set -- $M1; a1=("$@")
for k in 1 2 3 4 5; do
    s=${a0[$((k - 1))]}; e=${a1[$((k - 1))]}
    seg=$(sed -n "$((s + 1)),${e}p" "$WORK/n$k.log")
    sus=$(echo "$seg" | grep -ac -- '-> SUSPECT')
    ss=$(echo "$seg" | grep -ac 'SELF-SUSPICION')
    wf=$(echo "$seg" | grep -ac 'want table full')
    an=$(echo "$seg" | grep -ac '\[pfs\] announce id=')
    echo "  n$k L2 window: SUSPECT $sus SELF-SUSPICION $ss want-full $wf announces $an (2 rounds)"
    [ "$sus" = 0 ] && [ "$ss" = 0 ] && [ "$wf" = 0 ] && [ "$an" -le 128 ] || { l2=0; l2why="$l2why n$k"; }
done
if [ "$l2" = 1 ]; then res L2 PASS "no suspicion, no full want table, announces <= 64 a round"; else res L2 FAIL "on$l2why"; fi
# L3: node 5 stops after G1+1's merge, before G1+2's head
G3=$(( G2 + 1 ))
say 5 "stop now"
sleep 2; say 5 "stop"
wait_for "$WORK/n5.log" 'stopped 1' 10 || echo "[b5w-live] note: node 5 status line not seen"
for k in 1 2 3 4; do wait_for "$WORK/n$k.log" "\[baby\] auto: gen $G3 " $(( P + 30 )) || true; done
sleep 3
l3=1; l3why=""
n5=$(grep -ac "\[baby\] auto: \(publish \)\?gen $G3\b" "$WORK/n5.log")
[ "$n5" = 0 ] || { l3=0; l3why="node 5 has $n5 line(s) of gen $G3"; }
for k in 1 2 3 4; do
    ln=$(grep -a "\[baby\] auto: gen $G3 " "$WORK/n$k.log" | head -1)
    echo "  n$k: ${ln:-<none for gen $G3>}"
    echo "$ln" | grep -q "same-gen 4 " || { l3=0; l3why="$l3why n$k"; }
done
if [ "$l3" = 1 ]; then res L3 PASS "node 5 silent in gen $G3, the others same-gen 4"; else res L3 FAIL "$l3why"; fi
for k in 1 2 3 4 5; do say $k exit; done
sleep 2
if [ -z "$WANT_RED" ]; then
    if [ -z "$REDS" ]; then echo "[b5w-live] ALL PASS"; exit 0; fi
    echo "[b5w-live] RED:$REDS"; exit 1
fi
if [ "$REDS" = " $WANT_RED" ]; then echo "[b5w-live] falsifier $MODE: red on $WANT_RED only (as it must)"; exit 0; fi
echo "[b5w-live] falsifier $MODE: FAIL, red on '$REDS', want ' $WANT_RED'"; exit 1
