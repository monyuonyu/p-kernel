#!/bin/bash
# tests/host/run_n2c_wide.sh — N2c stage 3: a wide id from the key, a redraw when two keys
# claim one id, and "the same id is the same key" (docs/architecture/30-module/research/
# n2c-identity.md §2 2, 3, 5; checks I1w, I3, I4, I6 of §3; how it is built: §11).
#
# Expectations fixed before the code (2026-11-21, test first; red on master 1bccefa3):
#   I1w  a node with PKERNEL_NODE_WIDE=1, PKERNEL_PFS_DIR and no PKERNEL_NODE_ID says
#        "[n2c] wide id <N> (key, redraw 0)" with 257 <= N <= 65534, the same N on a second
#        start with the same dir, and a different N with another dir (another key)
#   I6u  `nodes idpin` (crafted v2 packets on the wide table, saved and put back) ends
#        "[idpin] ALL PASS": the first key heard for a wide id is kept, the same key changes
#        nothing, another key is a new node (its round trip is dropped, one line), the news
#        of an id carries the key kept for it, and news of this node's own id with another key
#        asks for a redraw only when this node's key is the larger one
#   live (relay + narrow 4 + two wide nodes A and B that both start as 300 with their own
#   keys: PKERNEL_N2C_TEST_WIDE0=300 replaces the drawn id for redraw 0 only):
#   I3   within 30 s of both being up exactly one of A and B says "[n2c] redraw" and comes
#        back as "[n2c] wide id M (key, redraw 1)" with M != 300; the other stays 300; and
#        then node 4 sees both 299 and M-1 ALIVE (`nodes` prints ids from 0) within 45 s
#   I6   node 4 said "[swim] wide node 299: another key" at least once (two keys behind
#        one id are two nodes), and no such line in the 20 s after I3 settled
#   I4   the one that redrew, killed with -9 and started again on the same dir, comes up
#        as M with "redraw 1" and no new "[n2c] redraw" line in 20 s
# Falsifiers (each must turn its check red; plain must be ALL PASS):
#   -DN2C_NC_NO_REDRAW -> I3    -DN2C_NC_REDRAW_VOLATILE -> I4    -DN2C_NC_NO_PIN -> I6u
# Not checked here (limits, §11): that a teaching (X3) is not mixed up between A and B;
# narrow ids (no key on the v1 wire); Android and Windows (no re-exec: the new id from the
# next start).
# N2C_WIDE_MODES (default all four), N2C_WIDE_PORT (default 7495), N2C_WORK (keep the logs).
# Output: "[n2c-wide] <mode> <check> PASS|FAIL ...", then ALL PASS (exit 0) or RED.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    *)             BOOT="$ROOT/boot/linux_x86_64" ;;
esac
unset PKERNEL_RELAY PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN PKERNEL_AUTONET \
      PKERNEL_NODE_ID PKERNEL_NODE_SEED PKERNEL_PFS_DIR PKERNEL_NODE_WIDE PKERNEL_N2C_TEST_WIDE0 \
      PKERNEL_RTT_ZONE_SIZE
export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
PORT="${N2C_WIDE_PORT:-7495}"
REL="$ROOT/relay/relay"
MODES="${N2C_WIDE_MODES:-plain N2C_NC_NO_REDRAW N2C_NC_REDRAW_VOLATILE N2C_NC_NO_PIN}"
if [ -n "${N2C_WORK:-}" ]; then
    WORK="$N2C_WORK"; mkdir -p "$WORK"
else
    WORK="$(mktemp -d /tmp/n2cwide.XXXXXX)"
fi
declare -A PID FD
RH=0; NEXTFD=10
stop_all() {
    for k in "${!PID[@]}"; do kill -9 "${PID[$k]}" 2>/dev/null; done
    for k in "${!FD[@]}"; do eval "exec ${FD[$k]}>&-" 2>/dev/null; done
    [ "$RH" != 0 ] && kill -9 "$RH" 2>/dev/null
    wait 2>/dev/null
    PID=(); FD=(); RH=0
}
cleanup() {
    stop_all
    make -C "$BOOT" clean >/dev/null 2>&1
    [ -z "${N2C_WORK:-}" ] && rm -rf "$WORK"
}
trap cleanup EXIT

make -C "$ROOT/relay" > "$WORK/relay-build.log" 2>&1 || { echo "[n2c-wide] relay build FAIL"; exit 2; }

build() {  # <cflags>
    make -C "$BOOT" clean >/dev/null 2>&1
    make -C "$BOOT" EXTRA_CFLAGS="$1" > "$WORK/build.log" 2>&1
}
run1() {  # <dir> <log> <commands...>: one node, a relay port nothing listens on
    local d="$1" log="$2"; shift 2
    printf '%s\n' "$@" exit > "$WORK/in.txt"
    timeout 60 env PKERNEL_PFS_DIR="$d" PKERNEL_NODE_WIDE=1 PKERNEL_RELAY="127.0.0.1:$((PORT + 1))" \
        PKERNEL_RELAY_AUTOFALLBACK=0 PKERNEL_AUTONET=1 "$BOOT/p-kernel" < "$WORK/in.txt" > "$log" 2>&1
}
# the last "[n2c] wide id N (key, redraw K)" line of a log: "N K", empty if none
wideline() { tr -d '\r' < "$1" | sed -n 's/^\[n2c\] wide id \([0-9]*\) (key, redraw \([0-9]*\)).*/\1 \2/p' | tail -1; }
firstwide() { tr -d '\r' < "$1" | sed -n 's/^\[n2c\] wide id \([0-9]*\) (key, redraw \([0-9]*\)).*/\1 \2/p' | head -1; }
cnt() { local c; c=$(grep -ac -- "$2" "$1" 2>/dev/null); echo "${c:-0}"; }

start() {  # <name> <dir> <env...>
    local n="$1" d="$2" fifo="$WORK/f-$1"; shift 2
    rm -f "$fifo"; mkfifo "$fifo"; mkdir -p "$d"
    env PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$d" PKERNEL_RELAY="127.0.0.1:$PORT" "$@" \
        "$BOOT/p-kernel" < "$fifo" >> "$LOG/$n.log" 2>&1 &
    PID[$n]=$!
    FD[$n]=$NEXTFD
    eval "exec $NEXTFD<>\"$fifo\""
    NEXTFD=$((NEXTFD + 1))
}
send() { printf '%s\n' "$2" >&"${FD[$1]}"; }
state_of() {  # <name> <row>: the state of row in the node's last `nodes` table
    tr -d '\r' < "$LOG/$1.log" \
        | awk -v r="$2" '/^\[cluster\]  ID/{s=""; next} $1==r && /^ +[0-9]+  [A-Z]+/{s=$2} END{print s}'
}
wait_up() {  # <name> <count of "mind_net_task up" lines to wait for>
    local n=0
    while [ "$n" -lt 240 ] && [ "$(cnt "$LOG/$1.log" 'mind_net_task up')" -lt "$2" ]; do sleep 0.5; n=$((n + 1)); done
}

suite() {  # <tag> <cflags>; sets SUITE_REDS
    local tag="$1" cf="$2" reds="" a b c t ra rb who other m k
    r() { echo "[n2c-wide] $tag $1 $2 ${3:-}"; [ "$2" = FAIL ] && reds="$reds $1"; return 0; }
    if ! build "$cf"; then r build FAIL; tail -20 "$WORK/build.log"; SUITE_REDS=build; return; fi
    LOG="$WORK/$tag"; rm -rf "$LOG"; mkdir -p "$LOG"

    # I1w
    run1 "$LOG/d1" "$LOG/w1.log" "sign key"
    run1 "$LOG/d1" "$LOG/w2.log" "sign key"
    run1 "$LOG/d2" "$LOG/w3.log" "sign key"
    a=$(wideline "$LOG/w1.log"); b=$(wideline "$LOG/w2.log"); c=$(wideline "$LOG/w3.log")
    if [ -n "$a" ] && [ "$a" = "$b" ] && [ -n "$c" ] && [ "${a% *}" != "${c% *}" ] \
       && [ "${a#* }" = 0 ] && [ "${a% *}" -ge 257 ] && [ "${a% *}" -le 65534 ]; then
        r I1w PASS "($a / $b / other dir $c)"; else r I1w FAIL "('$a' / '$b' / other dir '$c')"; fi

    # I6u
    run1 "$LOG/d1" "$LOG/u.log" "nodes idpin"
    if grep -aq '^\[idpin\] ALL PASS' "$LOG/u.log"; then r I6u PASS
    else r I6u FAIL "($(grep -a '^\[idpin\]' "$LOG/u.log" | grep -av ' ok' | head -3 | tr -d '\r' | tr '\n' ' '))"; fi

    # live
    "$REL" -p "$PORT" > "$LOG/relay.log" 2>&1 &
    RH=$!
    sleep 1
    start C "$LOG/dC" PKERNEL_NODE_ID=4
    wait_up C 1
    start A "$LOG/dA" PKERNEL_NODE_WIDE=1 PKERNEL_N2C_TEST_WIDE0=300
    start B "$LOG/dB" PKERNEL_NODE_WIDE=1 PKERNEL_N2C_TEST_WIDE0=300
    wait_up A 1; wait_up B 1
    t=0
    while [ "$t" -lt 30 ]; do
        sleep 1; t=$((t + 1))
        ra=$(cnt "$LOG/A.log" '^\[n2c\] redraw'); rb=$(cnt "$LOG/B.log" '^\[n2c\] redraw')
        [ $((ra + rb)) -ge 1 ] && break
    done
    sleep 5    # a second redraw (both sides) would show by now
    ra=$(cnt "$LOG/A.log" '^\[n2c\] redraw'); rb=$(cnt "$LOG/B.log" '^\[n2c\] redraw')
    who=""; other=""
    [ "$ra" = 1 ] && [ "$rb" = 0 ] && { who=A; other=B; }
    [ "$ra" = 0 ] && [ "$rb" = 1 ] && { who=B; other=A; }
    m=""; k=""
    if [ -n "$who" ]; then wait_up "$who" 2; m=$(wideline "$LOG/$who.log"); k="${m#* }"; m="${m% *}"; fi
    if [ -n "$who" ] && [ "$k" = 1 ] && [ -n "$m" ] && [ "$m" != 300 ] && [ "$(wideline "$LOG/$other.log")" = "300 0" ]; then
        t=0
        while [ "$t" -lt 45 ]; do
            sleep 3; t=$((t + 3)); send C nodes; sleep 1
            [ "$(state_of C 299)" = ALIVE ] && [ "$(state_of C $((m - 1)))" = ALIVE ] && break
        done
        if [ "$(state_of C 299)" = ALIVE ] && [ "$(state_of C $((m - 1)))" = ALIVE ]; then
            r I3 PASS "($who redrew to $m in about ${t} s more; 4 sees 299 and $((m - 1)) ALIVE)"
        else r I3 FAIL "($who redrew to $m but 4 sees 299=$(state_of C 299) $((m - 1))=$(state_of C $((m - 1))))"; fi
    else
        r I3 FAIL "(redraw lines A=$ra B=$rb; A '$(wideline "$LOG/A.log")' B '$(wideline "$LOG/B.log")')"
    fi

    # I6
    c=$(cnt "$LOG/C.log" '^\[swim\] wide node 299: another key')
    sleep 20
    b=$(cnt "$LOG/C.log" '^\[swim\] wide node 299: another key')
    if [ "$c" -ge 1 ] && [ "$b" = "$c" ] && [ -n "$who" ]; then r I6 PASS "($c lines, none in the next 20 s)"
    else r I6 FAIL "($c lines, then $b)"; fi

    # I4
    if [ -n "$who" ] && [ -n "$m" ]; then
        kill -9 "${PID[$who]}" 2>/dev/null; wait "${PID[$who]}" 2>/dev/null
        eval "exec ${FD[$who]}>&-"; unset "PID[$who]" "FD[$who]"
        : > "$LOG/$who-2.log"
        mv "$LOG/$who.log" "$LOG/$who-1.log"
        start "$who" "$LOG/d$who" PKERNEL_NODE_WIDE=1 PKERNEL_N2C_TEST_WIDE0=300
        wait_up "$who" 1
        sleep 20
        a=$(firstwide "$LOG/$who.log")
        if [ "$a" = "$m 1" ] && [ "$(cnt "$LOG/$who.log" '^\[n2c\] redraw')" = 0 ]; then
            r I4 PASS "(back as $a)"
        else r I4 FAIL "(back as '$a', redraw lines $(cnt "$LOG/$who.log" '^\[n2c\] redraw'))"; fi
    else
        r I4 FAIL "(no single redraw to restart)"
    fi
    stop_all
    SUITE_REDS="$reds"
}

FAIL=0
for md in $MODES; do
    if [ "$md" = plain ]; then
        suite plain ""
        [ -z "$SUITE_REDS" ] || { echo "[n2c-wide] plain red:$SUITE_REDS"; FAIL=1; }
        continue
    fi
    case "$md" in
        N2C_NC_NO_REDRAW)       want=I3 ;;
        N2C_NC_REDRAW_VOLATILE) want=I4 ;;
        N2C_NC_NO_PIN)          want=I6u ;;
        *) echo "[n2c-wide] unknown mode $md"; FAIL=1; continue ;;
    esac
    suite "$md" "-D$md"
    case " $SUITE_REDS " in
        *" $want "*) echo "[n2c-wide] $md turns $want red (as it must)" ;;
        *) echo "[n2c-wide] $md does NOT turn $want red"; FAIL=1 ;;
    esac
done
if [ "$FAIL" = 0 ]; then echo "[n2c-wide] ALL PASS"; exit 0; fi
echo "[n2c-wide] RED"; exit 1
