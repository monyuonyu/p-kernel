#!/bin/bash
# tests/host/run_n2_mix.sh — [n2-mix] old and new binaries in one swarm (ROADMAP N2,
# research/n2-wire16.md section 4). The minimum so far: X1 and X2, on the relay.
#
#   old 1,2,3 (N2_OLD_BIN) + new narrow 4,44 + new wide 260,300 (low bytes 4 and 44)
#   X1  the old nodes know {1,2,3,4,44} and nothing else (no phantom)
#   X2  the new narrow node knows all 7, 260 and 300 by their own numbers
#
# N2_OLD_BIN  a p-kernel built from the last tree before N2 wire changes (master 64b7c909;
#             record the sha with any result). N2_NEW_BIN: default boot/linux_x86_64/p-kernel,
#             so an -DN2_NC_TRUNC build goes in there. N2_RELAY_BIN: default relay/relay.
# N2_WAIT     seconds to wait for membership, default 120. N2_LOGDIR keeps the logs.
# N2_NO_WIDE=1  leave 260 and 300 out: old and new narrow only, must be green already.
# N2_RELAY_OLD=1  the relay is an old one (N2_RELAY_BIN): 260 and 300 run but cannot get in. X1
#             and X2 then want the five narrow ids: a wide node on an old relay disturbs nobody.
#
# `nodes` prints ids from 0 (PKERNEL_NODE_ID 44 is row 43).
#
# Negative control: -DN2_NC_TRUNC cuts the wide ids to a byte, so 260 and 300 become 4 and 44.
# 260 and 300 are in SWIM's membership only (swim.c): X3 and X4 of the design are not written
# yet, because no other subsystem sees a wide id.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OLD="${N2_OLD_BIN:?N2_OLD_BIN: a p-kernel from before N2}"
NEW="${N2_NEW_BIN:-$ROOT/boot/linux_x86_64/p-kernel}"
REL="${N2_RELAY_BIN:-$ROOT/relay/relay}"
WAIT="${N2_WAIT:-120}"
NOWIDE="${N2_NO_WIDE:-0}"
PORT="${N2_PORT:-7595}"
[ -x "$OLD" ] && [ -x "$NEW" ] && [ -x "$REL" ] || { echo "[n2-mix] missing a binary"; exit 2; }

export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
unset PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN PKERNEL_RTT_ZONE_SIZE
WORK="$(mktemp -d /tmp/n2mix.XXXXXX)"
LOGDIR="${N2_LOGDIR:-$WORK/logs}"; mkdir -p "$LOGDIR"
declare -A PID FD
RH=0; REDS=""; NEXTFD=10

res() { echo "[n2-mix] $1 $2: $3"; [ "$2" = FAIL ] && REDS="$REDS $1"; return 0; }
cleanup() {
    for k in "${!PID[@]}"; do kill -9 "${PID[$k]}" 2>/dev/null; done
    [ "$RH" != 0 ] && kill -9 "$RH" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"/d* "$WORK"/f*
    [ -z "${N2_LOGDIR:-}" ] && rm -rf "$WORK"
}
trap cleanup EXIT

start() {  # <binary> <id>
    local bin="$1" i="$2" fifo="$WORK/f$2"
    mkfifo "$fifo"; mkdir -p "$WORK/d$i"
    env PKERNEL_NODE_ID="$i" PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$WORK/d$i" \
        PKERNEL_RELAY="127.0.0.1:$PORT" "$bin" < "$fifo" > "$LOGDIR/node$i.log" 2>&1 &
    PID[$i]=$!
    FD[$i]=$NEXTFD
    eval "exec $NEXTFD<>\"$fifo\""
    NEXTFD=$((NEXTFD + 1))
}
send() { printf '%s\n' "$2" >&"${FD[$1]}"; }
# The ids of the last `nodes` table (swim_nodes_print) of a log, alive or self, sorted.
known() {
    tr -d '\r' < "$LOGDIR/node$1.log" \
        | awk '/^\[cluster\]  ID/{s=""; next} /^ +[0-9]+  (SELF|ALIVE)/{s=s" "$1} END{print s}' \
        | tr ' ' '\n' | grep -E '^[0-9]+$' | sort -n | tr '\n' ' '
}

"$REL" -p "$PORT" > "$LOGDIR/relay.log" 2>&1 &
RH=$!
sleep 1
for i in 1 2 3; do start "$OLD" "$i"; sleep 0.2; done
WIDE="260 300"; [ "$NOWIDE" = 1 ] && WIDE=""
for i in 4 44 $WIDE; do start "$NEW" "$i"; sleep 0.2; done
for i in 1 2 3 4 44 $WIDE; do
    n=0
    while [ "$n" -lt 240 ] && ! grep -aq 'mind_net_task up' "$LOGDIR/node$i.log"; do sleep 0.5; n=$((n + 1)); done
    send "$i" "stop now"
done

WANT_A="0 1 2 3 43 "; WANT_B="0 1 2 3 43 259 299 "
[ "$NOWIDE" = 1 ] && WANT_B="$WANT_A"
[ "${N2_RELAY_OLD:-0}" = 1 ] && WANT_B="$WANT_A"
t=0; a=""; b=""
while [ "$t" -lt "$WAIT" ]; do
    sleep 3; t=$((t + 3))
    send 1 "nodes"; send 4 "nodes"; sleep 1
    a="$(known 1)"; b="$(known 4)"
    [ "$a" = "$WANT_A" ] && [ "$b" = "$WANT_B" ] && break
done

[ "$a" = "$WANT_A" ] && res X1 PASS "old node 1 knows {$a}" || res X1 FAIL "old node 1 knows {$a}, wanted {$WANT_A}"
[ "$b" = "$WANT_B" ] && res X2 PASS "new node 4 knows {$b}" || res X2 FAIL "new node 4 knows {$b}, wanted {$WANT_B}"

if [ -z "$REDS" ]; then echo "[n2-mix] ALL PASS"; exit 0; fi
echo "[n2-mix] RED:$REDS"
exit 1
