#!/bin/bash
# tests/host/run_mind_gen0_b.sh — MIND-GEN-0-B, partition and reconnect
# (ROADMAP 1-2). The design and the PRE-REGISTERED expectations E1..E5 are in
# docs/architecture/30-module/mind-gen0-b.md; they were written before this
# script first ran and are not tuned to its results.
#
#   P1 = nodes 1,2 on relays "R1,RH"   P2 = nodes 3,4,5 on "R2,RH"
#   node1 sky->blue, node2 fire->warm | node3 sky->green, node4 snow->white
#   sleep; kill -9 nodes 2 and 4 (the teachers of fire and snow)
#   heal: kill -9 R1 and R2 -> everyone fails over to RH
#   node 6 (Z) joins on RH only; ask 1,3,5,Z for fire, snow, sky; again 30 s later
#
# Env: MG0B_NC=1 skips the heal (negative control: E1 must go red).
#      MG0B_PORT (default 7471; uses PORT, PORT+1, PORT+2), MG0B_LOGDIR.
# Output: "[mind-gen0-b] E<n> PASS|FAIL ..." per expectation, then
#   [mind-gen0-b] ALL PASS (exit 0) or [mind-gen0-b] RED: <list> (exit 1).
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    *)             BOOT="$ROOT/boot/linux_x86_64" ;;
esac
[ -x "$BOOT/p-kernel" ]    || make -C "$BOOT"       >/dev/null || exit 1
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay" >/dev/null || exit 1

export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
unset PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN
PORT="${MG0B_PORT:-7471}"; R1P=$PORT; R2P=$((PORT + 1)); RHP=$((PORT + 2))
NC="${MG0B_NC:-0}"
WORK="$(mktemp -d /tmp/mg0b.XXXXXX)"
LOGDIR="${MG0B_LOGDIR:-$WORK/logs}"; mkdir -p "$LOGDIR"
declare -A PID
R1=0; R2=0; RH=0
REDS=""

TS()  { date '+%H:%M:%S'; }
note() { echo "[$(TS)] $*"; }
res() {  # <E> <PASS|FAIL> <text>
    echo "[mind-gen0-b] $1 $2: $3"
    [ "$2" = FAIL ] && REDS="$REDS $1"
}
cleanup() {
    for i in 1 2 3 4 5 6; do
        [ "${PID[$i]:-0}" != 0 ] && kill -9 "${PID[$i]}" 2>/dev/null
        eval "exec $((i + 2))>&-" 2>/dev/null
    done
    for p in "$R1" "$R2" "$RH"; do [ "$p" != 0 ] && kill -9 "$p" 2>/dev/null; done
    wait 2>/dev/null
}
trap cleanup EXIT

start_relay() {  # <port> -> sets LAST_RELAY
    "$ROOT/relay/relay" -p "$1" > "$LOGDIR/relay-$1.log" 2>&1 &
    LAST_RELAY=$!
}
start_node() {  # <i> <relay list>
    local i="$1" fifo="$WORK/f$1"
    rm -f "$fifo"; mkfifo "$fifo"; mkdir -p "$WORK/d$i"
    env PKERNEL_NODE_ID="$i" PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$WORK/d$i" PKERNEL_RELAY="$2" \
        "$BOOT/p-kernel" < "$fifo" > "$LOGDIR/node$i.log" 2>&1 &
    PID[$i]=$!
    eval "exec $((i + 2))<>\"$fifo\""
    note "node$i up pid=${PID[$i]} relays=$2"
}
send() { local i="$1"; shift; printf '%s\n' "$*" >&$((i + 2)); }
wait_for() {  # <file> <ERE> <secs>
    local n=0
    while [ "$n" -lt "$(( $3 * 2 ))" ]; do
        grep -aqE "$2" "$1" 2>/dev/null && return 0
        sleep 0.5; n=$((n + 1))
    done
    return 1
}
region_size() {  # <node> <size> <secs>: poll `region` until "size=<size>" appears after now
    local i="$1" lg="$LOGDIR/node$1.log" t=0 pre
    pre=$(grep -ac "size=$2" "$lg")
    while [ "$t" -lt "$3" ]; do
        send "$i" "region"; sleep 2; t=$((t + 2))
        [ "$(grep -ac "size=$2" "$lg")" -gt "$pre" ] && return 0
    done
    return 1
}
ANS=""; TAUGHT=""
ask() {  # <node> <key>: sets ANS (answer word) and TAUGHT (teacher node or "local")
    local i="$1" k="$2" lg="$LOGDIR/node$1.log" pre n=0
    pre=$(grep -ac "ask \"$k\" ->" "$lg")
    send "$i" "mind ask $k"
    while [ "$(grep -ac "ask \"$k\" ->" "$lg")" -le "$pre" ] && [ "$n" -lt 60 ]; do sleep 0.5; n=$((n + 1)); done
    ANS=$(grep -a "ask \"$k\" ->" "$lg" | tail -1 | grep -aoE -- '-> "[a-z]+"' | tr -d '">-' | tr -d ' ')
    sleep 0.5
    TAUGHT=$(grep -a -A8 "ask \"$k\" ->" "$lg" | tail -9 | grep -aoE 'taught by node [0-9]+' | tail -1 | grep -aoE '[0-9]+$')
    [ -z "$TAUGHT" ] && TAUGHT=local
}

echo "==========================================================="
echo " MIND-GEN-0-B — partition, independent learning, deaths, heal, new joiner"
echo " logs: $LOGDIR   negative control (no heal): $NC"
echo "==========================================================="
start_relay "$R1P"; R1=$LAST_RELAY
start_relay "$R2P"; R2=$LAST_RELAY
start_relay "$RHP"; RH=$LAST_RELAY
sleep 1
L1="127.0.0.1:$R1P,127.0.0.1:$RHP"; L2="127.0.0.1:$R2P,127.0.0.1:$RHP"
for i in 1 2; do start_node "$i" "$L1"; done
for i in 3 4 5; do start_node "$i" "$L2"; done
for i in 1 2 3 4 5; do wait_for "$LOGDIR/node$i.log" 'mind_net_task up' 90 || note "node$i: mind_net_task not seen"; done

region_size 1 2 150 && note "P1 formed a region (size=2)" || note "P1 region size=2 not seen"
region_size 3 3 150 && note "P2 formed a region (size=3)" || note "P2 region size=3 not seen"
grep -aq "size=[3-9]" "$LOGDIR/node1.log" && note "WARNING: node1 saw a region larger than P1 — the partition leaked"

# ---- phase 1: learn apart --------------------------------------------------
send 1 "mind teach sky blue";   send 2 "mind teach fire warm"
send 3 "mind teach sky green";  send 4 "mind teach snow white"
for i in 1 2 3 4; do wait_for "$LOGDIR/node$i.log" 'published mind/teach' 120 || note "node$i never published"; done
for i in 1 2 3 4 5; do send "$i" "mind wait 90"; done
for i in 1 2 3 4 5; do wait_for "$LOGDIR/node$i.log" 'wait: drained|distilled in-context facts' 200 || note "node$i: no drain/distill line"; done
sleep 5
# ---- phase 2: deaths ------------------------------------------------------
note "kill -9 node2 (taught fire) and node4 (taught snow)"
kill -9 "${PID[2]}" "${PID[4]}"; PID[2]=0; PID[4]=0
# ---- phase 3: heal ---------------------------------------------------------
if [ "$NC" = 1 ]; then
    note "NEGATIVE CONTROL: no heal (R1 and R2 stay up)"
else
    note "heal: kill -9 R1 and R2 -> failover to RH"
    kill -9 "$R1" "$R2"; R1=0; R2=0
    for i in 1 3 5; do wait_for "$LOGDIR/node$i.log" "failover -> relay#1 127.0.0.1:$RHP" 40 || note "node$i: no failover line"; done
fi
region_size 1 3 150 && note "healed region size=3 seen on node1" || note "node1 never saw size=3"
start_node 6 "127.0.0.1:$RHP"
wait_for "$LOGDIR/node6.log" 'mind_net_task up' 90 || note "Z: mind_net_task not seen"
region_size 6 4 150 && note "Z joined (size=4)" || note "Z never saw size=4"
sleep 20

# ---- phase 4: ask ----------------------------------------------------------
declare -A A1 T1 A2
for r in 1 2; do
    [ "$r" = 2 ] && { note "second round in 30 s"; sleep 30; }
    for i in 1 3 5 6; do
        for k in fire snow sky; do
            ask "$i" "$k"
            note "round$r node$i ask $k -> ${ANS:-<none>} (taught: $TAUGHT)"
            if [ "$r" = 1 ]; then A1[$i.$k]="$ANS"; T1[$i.$k]="$TAUGHT"; else A2[$i.$k]="$ANS"; fi
        done
    done
done

# E1: both sides' learning survives, on every live node including Z
e1=""
for i in 1 3 5 6; do
    [ "${A1[$i.fire]}" = warm ]  || e1="$e1 node$i:fire=${A1[$i.fire]:-none}"
    [ "${A1[$i.snow]}" = white ] || e1="$e1 node$i:snow=${A1[$i.snow]:-none}"
done
[ -z "$e1" ] && res E1 PASS "fire->warm and snow->white on 1,3,5,Z" || res E1 FAIL "missing:$e1"
# E2: one answer for sky everywhere
s=$(for i in 1 3 5 6; do echo "${A1[$i.sky]:-none}"; done | sort -u | tr '\n' ' ')
[ "$(echo $s | wc -w)" = 1 ] && [ "$s" != "none " ] && res E2 PASS "every node answers sky -> $s" \
    || res E2 FAIL "sky answers differ or missing: 1=${A1[1.sky]:-none} 3=${A1[3.sky]:-none} 5=${A1[5.sky]:-none} Z=${A1[6.sky]:-none}"
# E3: the named teacher matches the value (blue <- 1, green <- 3; local = self)
e3=""
for i in 1 3 5 6; do
    a="${A1[$i.sky]:-none}"; t="${T1[$i.sky]:-local}"; [ "$t" = local ] && t=$i
    case "$a" in
        blue)  [ "$t" = 1 ] || e3="$e3 node$i:blue-by-$t" ;;
        green) [ "$t" = 3 ] || e3="$e3 node$i:green-by-$t" ;;
        *)     e3="$e3 node$i:$a" ;;
    esac
done
[ -z "$e3" ] && res E3 PASS "sky's teacher matches its value on every node" || res E3 FAIL "$e3"
# E4: the losing value and its teacher remain readable — no such read exists today
# (ROADMAP 1-3). The script looks for a "conflict" record in `mind ask` output.
e4=0
for i in 1 3 5 6; do grep -aqE '^\[mind\] +conflict' "$LOGDIR/node$i.log" && e4=$((e4 + 1)); done
[ "$e4" = 4 ] && res E4 PASS "a conflict record is readable on every node" \
    || res E4 FAIL "no readable conflict record on $((4 - e4)) of 4 nodes (only the remote REVISE log line and a galaxy event exist)"
# E5: no resurrection 30 s later
e5=""
for i in 1 3 5 6; do [ "${A1[$i.sky]:-none}" = "${A2[$i.sky]:-none}" ] || e5="$e5 node$i:${A1[$i.sky]:-none}->${A2[$i.sky]:-none}"; done
[ -z "$e5" ] && res E5 PASS "sky answers unchanged after 30 s" || res E5 FAIL "changed:$e5"

grep -ah "remote REVISE" "$LOGDIR"/node*.log | sed 's/^/[mind-gen0-b]   revise: /' | head -8
if [ -z "$REDS" ]; then echo "[mind-gen0-b] ALL PASS"; exit 0; fi
echo "[mind-gen0-b] RED:$REDS"
exit 1
