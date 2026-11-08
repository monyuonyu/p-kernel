#!/bin/bash
# tests/host/run_n2_alive.sh — [n2-alive] who can tell that a peer died, across narrow and wide
# ids (ROADMAP N2, research/n2-wire16.md "まだ" 4). New binaries only, on the relay:
#
#   narrow 4, 44, 5 + wide 260, 300 (low bytes 4 and 44; `nodes` prints ids from 0)
#   A0  for N2_STEADY seconds (default 30) with everyone up, nobody marks a live peer
#       SUSPECT or DEAD (no false death: the cost side of any liveness change)
#   then 44 (a narrow node) and 300 (a wide node) are killed with -9
#   A1  narrow 4 sees 43 DEAD               (v1 probing; the control, green before N2)
#   A2  wide 260 sees 43 DEAD               (a wide node can let a narrow peer die)
#   A3  narrow 4 sees 299 DEAD              (a wide peer dies on a narrow node)
#   A4  wide 260 sees 299 DEAD              (and on a wide node)
#   A5  4 and 260 still see 4 (row 4 = id 5), 3 and 259 ALIVE (only the killed ones died)
#   A6  no node logs "misdelivered" (UDP receivers; test-holes H20). Negative control: a relay
#       built with -DRELAY_NC_MISROUTE (frames for 300 go to 260) reddens A6
#
# Written test first for "まだ" 4 ("a wide node never lets a narrow peer die"): green on
# master e72de183 (2 runs, 11-08). A wide self marks a dead narrow peer by its own failing
# v1 probe (SUSPECT) and the narrow peers' DEAD gossip; a dead wide peer ages out of its
# beacons (SWIM_WIDE_DEAD_MS). So this is a guard of today's behaviour, not a red to fix.
# Negative control: -DN2_NC_WIDE_NEVER_DEAD (a wide peer stops at SUSPECT) reddens A3 and A4.
# Limit: A2 has two paths (own probe, gossip) and no negative takes out either one.
#
# N2_NEW_BIN (default boot/linux_x86_64/p-kernel), N2_RELAY_BIN (default relay/relay),
# N2_WAIT (default 60: seconds for membership, and again for the deaths), N2_LOGDIR, N2_PORT.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
NEW="${N2_NEW_BIN:-$ROOT/boot/linux_x86_64/p-kernel}"
REL="${N2_RELAY_BIN:-$ROOT/relay/relay}"
WAIT="${N2_WAIT:-60}"
STEADY="${N2_STEADY:-30}"
PORT="${N2_PORT:-7597}"
[ -x "$NEW" ] && [ -x "$REL" ] || { echo "[n2-alive] missing a binary"; exit 2; }

export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
unset PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN PKERNEL_RTT_ZONE_SIZE
WORK="$(mktemp -d /tmp/n2alive.XXXXXX)"
LOGDIR="${N2_LOGDIR:-$WORK/logs}"; mkdir -p "$LOGDIR"
declare -A PID FD
RH=0; REDS=""; NEXTFD=10
IDS="4 44 5 260 300"

res() { echo "[n2-alive] $1 $2: $3"; [ "$2" = FAIL ] && REDS="$REDS $1"; return 0; }
cleanup() {
    for k in "${!PID[@]}"; do kill -9 "${PID[$k]}" 2>/dev/null; done
    [ "$RH" != 0 ] && kill -9 "$RH" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"/d* "$WORK"/f*
    [ -z "${N2_LOGDIR:-}" ] && rm -rf "$WORK"
}
trap cleanup EXIT

start() {  # <id>
    local i="$1" fifo="$WORK/f$1"
    mkfifo "$fifo"; mkdir -p "$WORK/d$i"
    env PKERNEL_NODE_ID="$i" PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$WORK/d$i" \
        PKERNEL_RELAY="127.0.0.1:$PORT" "$NEW" < "$fifo" > "$LOGDIR/node$i.log" 2>&1 &
    PID[$i]=$!
    FD[$i]=$NEXTFD
    eval "exec $NEXTFD<>\"$fifo\""
    NEXTFD=$((NEXTFD + 1))
}
send() { printf '%s\n' "$2" >&"${FD[$1]}"; }
# the state of row $2 in node $1's last `nodes` table (narrow rows and wide rows alike)
state_of() {
    tr -d '\r' < "$LOGDIR/node$1.log" \
        | awk -v r="$2" '/^\[cluster\]  ID/{s=""; next} $1==r && /^ +[0-9]+  [A-Z]+/{s=$2} END{print s}'
}
refresh() { for i in "$@"; do send "$i" "nodes"; done; sleep 1; }

"$REL" -p "$PORT" > "$LOGDIR/relay.log" 2>&1 &
RH=$!
sleep 1
for i in $IDS; do start "$i"; sleep 0.2; done
for i in $IDS; do
    n=0
    while [ "$n" -lt 240 ] && ! grep -aq 'mind_net_task up' "$LOGDIR/node$i.log"; do sleep 0.5; n=$((n + 1)); done
done

# membership: 4 and 260 both see 3 4 43 259 299 ALIVE
all_alive() {  # node $1 sees every row after it ALIVE
    local n="$1" r; shift
    for r in "$@"; do [ "$(state_of "$n" "$r")" = ALIVE ] || return 1; done
}
t=0
while [ "$t" -lt "$WAIT" ]; do
    sleep 2; t=$((t + 2)); refresh 4 260
    all_alive 4 4 43 259 299 && all_alive 260 3 4 43 299 && break
done
if ! { all_alive 4 4 43 259 299 && all_alive 260 3 4 43 299; }; then
    res A0 FAIL "no full membership after ${WAIT} s (4: 4=$(state_of 4 4) 43=$(state_of 4 43) 259=$(state_of 4 259) 299=$(state_of 4 299);" \
               "260: 3=$(state_of 260 3) 4=$(state_of 260 4) 43=$(state_of 260 43) 299=$(state_of 260 299))"
    echo "[n2-alive] RED:$REDS"; exit 1
fi

# A0: steady, no false death anywhere (any "-> SUSPECT" or "-> DEAD" line from now on is false)
declare -A B0
for i in $IDS; do B0[$i]="$(grep -acE -- '-> (SUSPECT|DEAD)' "$LOGDIR/node$i.log")"; done
sleep "$STEADY"
fd=""
for i in $IDS; do
    c="$(grep -acE -- '-> (SUSPECT|DEAD)' "$LOGDIR/node$i.log")"
    [ "$c" -gt "${B0[$i]}" ] && fd="$fd node$i:+$((c - ${B0[$i]}))"
done
[ -z "$fd" ] && res A0 PASS "${STEADY} s with all five up: no SUSPECT or DEAD line" \
             || res A0 FAIL "a live peer was marked SUSPECT or DEAD:$fd"

kill -9 "${PID[44]}" "${PID[300]}" 2>/dev/null; unset 'PID[44]' 'PID[300]'
t=0
while [ "$t" -lt "$WAIT" ]; do
    sleep 3; t=$((t + 3)); refresh 4 260
    [ "$(state_of 4 43)" = DEAD ] && [ "$(state_of 260 43)" = DEAD ] \
        && [ "$(state_of 4 299)" = DEAD ] && [ "$(state_of 260 299)" = DEAD ] && break
done
s=$(state_of 4 43);   [ "$s" = DEAD ] && res A1 PASS "narrow 4 sees 43 DEAD (the wait ended at ${t} s)" || res A1 FAIL "narrow 4 sees 43 as '$s' after ${t} s"
s=$(state_of 260 43); [ "$s" = DEAD ] && res A2 PASS "wide 260 sees 43 DEAD" || res A2 FAIL "wide 260 sees 43 as '$s' after ${t} s"
s=$(state_of 4 299);  [ "$s" = DEAD ] && res A3 PASS "narrow 4 sees 299 DEAD" || res A3 FAIL "narrow 4 sees 299 as '$s' after ${t} s"
s=$(state_of 260 299); [ "$s" = DEAD ] && res A4 PASS "wide 260 sees 299 DEAD" || res A4 FAIL "wide 260 sees 299 as '$s' after ${t} s"
a5=""
for r in 4 259; do [ "$(state_of 4 "$r")" = ALIVE ] || a5="$a5 4:$r=$(state_of 4 "$r")"; done
for r in 3 4; do [ "$(state_of 260 "$r")" = ALIVE ] || a5="$a5 260:$r=$(state_of 260 "$r")"; done
[ -z "$a5" ] && res A5 PASS "the living stay ALIVE on 4 and 260" || res A5 FAIL "a living peer is not ALIVE:$a5"
# A6 (test-holes H20, the UDP side): no node got a one-node frame (REL_DATA) whose MAC names
# another node (relay_misdeliver.h). Before, only [n2-tcp] T5 looked at this line, and a relay
# that sent 300's frames to 260 left every line above green (audit-165).
md=""
for i in $IDS; do
    # a missing or empty log counts nothing (grep -c printed "" and the test fell to PASS, audit-166)
    [ -s "$LOGDIR/node$i.log" ] || { md="$md node$i:no-log"; continue; }
    c=$(grep -ac 'misdelivered: a frame for node' "$LOGDIR/node$i.log")
    [ "$c" -gt 0 ] && md="$md node$i:$c($(grep -a 'misdelivered' "$LOGDIR/node$i.log" | head -1 | sed 's/.*misdelivered: //'))"
done
[ -z "$md" ] && res A6 PASS "no one-node frame came to a node its MAC does not name" \
             || res A6 FAIL "misdelivered one-node frames:$md"

if [ -z "$REDS" ]; then echo "[n2-alive] ALL PASS"; exit 0; fi
echo "[n2-alive] RED:$REDS"
exit 1
