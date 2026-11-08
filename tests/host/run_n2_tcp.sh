#!/bin/bash
# tests/host/run_n2_tcp.sh — [n2-tcp] wide ids over the TCP relay transport (ROADMAP N2,
# research/n2-wire16.md "まだ" 6). One relay (it serves UDP and TCP on one table), the key on:
#
#   narrow 4 (UDP), narrow 44 (TCP), wide 260 (UDP), wide 300 (TCP); `nodes` prints ids from 0
#   T0  44 and 300 run on TCP ("transport = relay-tcp (node 44)", "(node 300)"): before, a wide
#       id was refused on TCP and fell back to UDP
#   T1  narrow 4 (UDP) sees 43, 259 and 299 ALIVE
#   T2  wide 300 (TCP) sees 3, 43 and 259 ALIVE (v3 and v2 frames come in over the stream)
#   T3  narrow 44 (TCP) sees 259 and 299 ALIVE: its frames say "I read v3" in byte 10, or the
#       relay never forwards a wide frame to it
#   T4  300 teaches snow: it arrives at 4, 44 and 260 from node 299 (kdds over v3 on TCP)
#
# Negative controls: -DN2_NC_TCP_WIDE_REFUSE (the old rule) reddens T0, -DN2_NC_TCP_NO_READS_V3
# (byte 10 stays 0) reddens T3.
# N2_NEW_BIN (default boot/linux_x86_64/p-kernel), N2_RELAY_BIN (default relay/relay),
# N2_WAIT (default 90), N2_LOGDIR, N2_PORT.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
NEW="${N2_NEW_BIN:-$ROOT/boot/linux_x86_64/p-kernel}"
REL="${N2_RELAY_BIN:-$ROOT/relay/relay}"
WAIT="${N2_WAIT:-90}"
PORT="${N2_PORT:-7599}"
[ -x "$NEW" ] && [ -x "$REL" ] || { echo "[n2-tcp] missing a binary"; exit 2; }

export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
unset PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN PKERNEL_RTT_ZONE_SIZE PKERNEL_SEED
export PKERNEL_REGION_TAU_MS="${PKERNEL_REGION_TAU_MS:-250}"
export PKERNEL_RELAY_AUTOFALLBACK=0
WORK="$(mktemp -d /tmp/n2tcp.XXXXXX)"
LOGDIR="${N2_LOGDIR:-$WORK/logs}"; mkdir -p "$LOGDIR"
declare -A PID FD
RH=0; REDS=""; NEXTFD=10
IDS="4 44 260 300"

res() { echo "[n2-tcp] $1 $2: $3"; [ "$2" = FAIL ] && REDS="$REDS $1"; return 0; }
cleanup() {
    for k in "${!PID[@]}"; do kill -9 "${PID[$k]}" 2>/dev/null; done
    [ "$RH" != 0 ] && kill -9 "$RH" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"/d* "$WORK"/f*
    [ -z "${N2_LOGDIR:-}" ] && rm -rf "$WORK"
}
trap cleanup EXIT

start() {  # <id> <tcp 0|1>
    local i="$1" fifo="$WORK/f$1"
    mkfifo "$fifo"; mkdir -p "$WORK/d$i"
    env PKERNEL_NODE_ID="$i" PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$WORK/d$i" PKERNEL_RELAY_TCP="$2" \
        PKERNEL_RELAY="127.0.0.1:$PORT" "$NEW" < "$fifo" > "$LOGDIR/node$i.log" 2>&1 &
    PID[$i]=$!
    FD[$i]=$NEXTFD
    eval "exec $NEXTFD<>\"$fifo\""
    NEXTFD=$((NEXTFD + 1))
}
send() { printf '%s\n' "$2" >&"${FD[$1]}"; }
state_of() {  # the state of row $2 in node $1's last `nodes` table
    tr -d '\r' < "$LOGDIR/node$1.log" \
        | awk -v r="$2" '/\[cluster\]  ID/{s=""; next} $1==r && /^ +[0-9]+  [A-Z]+/{s=$2} END{print s}'
}
all_alive() {  # node $1 sees every row after it ALIVE
    local n="$1" r; shift
    for r in "$@"; do [ "$(state_of "$n" "$r")" = ALIVE ] || return 1; done
}
states() {  # "r=STATE ..." of node $1 for the rows after it
    local n="$1" r o=""; shift
    for r in "$@"; do o="$o $r=$(state_of "$n" "$r")"; done
    echo "$o"
}

"$REL" -p "$PORT" > "$LOGDIR/relay.log" 2>&1 &
RH=$!
sleep 1
start 4 0; sleep 0.2; start 44 1; sleep 0.2; start 260 0; sleep 0.2; start 300 1
for i in $IDS; do
    n=0
    while [ "$n" -lt 240 ] && ! grep -aq 'mind_net_task up' "$LOGDIR/node$i.log"; do sleep 0.5; n=$((n + 1)); done
done

t0=""
for i in 44 300; do grep -aqF "transport = relay-tcp (node $i)" "$LOGDIR/node$i.log" || t0="$t0 $i"; done
[ -z "$t0" ] && res T0 PASS "44 and 300 run on TCP" \
             || res T0 FAIL "not on TCP:$t0 ($(grep -ah 'net_relay_tcp\] PKERNEL_NODE_ID\|transport =' "$LOGDIR/node300.log" | head -2 | tr '\n' '|'))"

t=0
while [ "$t" -lt "$WAIT" ]; do
    sleep 3; t=$((t + 3))
    for i in 4 44 300; do send "$i" "nodes"; done; sleep 1
    all_alive 4 43 259 299 && all_alive 300 3 43 259 && all_alive 44 259 299 && break
done
all_alive 4 43 259 299 && res T1 PASS "narrow 4 (UDP) sees 43, 259, 299" \
                       || res T1 FAIL "narrow 4 (UDP):$(states 4 43 259 299)"
all_alive 300 3 43 259 && res T2 PASS "wide 300 (TCP) sees 3, 43, 259" \
                       || res T2 FAIL "wide 300 (TCP):$(states 300 3 43 259)"
all_alive 44 259 299 && res T3 PASS "narrow 44 (TCP) sees 259 and 299" \
                     || res T3 FAIL "narrow 44 (TCP):$(states 44 259 299)"

arrived() { grep -aqE "remote teach arrived: \"$2\".* from node $3 " "$LOGDIR/node$1.log"; }
sleep 2
send 300 "mind teach snow white"
t=0; miss="?"
while [ "$t" -lt "$WAIT" ] && [ -n "$miss" ]; do
    sleep 3; t=$((t + 3)); miss=""
    for i in 4 44 260; do arrived "$i" snow 299 || miss="$miss snow@$i"; done
done
[ -z "$miss" ] && res T4 PASS "snow from 299 (TCP) at 4, 44, 260" || res T4 FAIL "missing:$miss"

if [ -z "$REDS" ]; then echo "[n2-tcp] ALL PASS"; exit 0; fi
echo "[n2-tcp] RED:$REDS"
exit 1
