#!/bin/bash
# tests/host/run_n2c_life.sh — N2c stage 2: a DEAD peer comes back only with a
# newer incarnation of its own (docs/architecture/30-module/research/n2c-identity.md
# §10; the checks and the falsifiers were fixed there before the code, audit-197
# added the conditions: I7 c as a test, "back at" counted in I5b).
#
#   I7  `nodes life` (one node, crafted packets): "[swim-life] PASS"
#   I8  relay + nodes 1,2,3. kill -STOP node 2 until nodes 1 and 3 say DEAD,
#       wait 20 s (past the gossip TTL), kill -CONT. Within 60 s both bring it
#       back with a "back at incarnation" line, and neither with a rx or probe
#       line ("node 1 recovered (via rx)" / "-> ALIVE (direct|indirect probe)")
#   I5b kill -9 node 2, 20 s after both DEAD lines restart it on the same
#       PFS dir. Both see it back within 60 s ("recovered", "-> ALIVE" or
#       "back at"). Two arms: the plain build (node.incarn kept) and
#       -DN2C_NC_INC_RESET (every start is incarnation 0)
# Falsifiers (the plain build must be ALL PASS):
#   -DN2C_NC_RX_REVIVE -> I7 (a1/a2/c) and I8 red
#   -DN2C_NC_NO_TELL   -> I7 (a1/a2 told) and I5b of the RESET arm red
#   -DN2C_NC_PLAIN_CMP -> I7 (d/e/f) red
# Output: "[n2c-life] <build> <check> PASS|FAIL", then ALL PASS (exit 0) or RED.
# N2C_LIFE_MODES picks the builds; N2C_WORK=<dir> keeps the logs.
# Rebuilds boot/linux* and relay/ and cleans boot/linux* afterwards.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    *)             BOOT="$ROOT/boot/linux_x86_64" ;;
esac
unset PKERNEL_RELAY PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN PKERNEL_AUTONET \
      PKERNEL_NODE_ID PKERNEL_NODE_SEED PKERNEL_PFS_DIR
PIDS=""
cleanup() {
    for p in $PIDS; do kill -9 "$p" 2>/dev/null; done
    make -C "$BOOT" clean >/dev/null 2>&1
}
if [ -n "${N2C_WORK:-}" ]; then
    WORK="$N2C_WORK"; mkdir -p "$WORK"
    trap 'cleanup' EXIT
else
    WORK="$(mktemp -d /tmp/n2clife.XXXXXX)"
    trap 'cleanup; rm -rf "$WORK"' EXIT
fi
PORT="${N2C_LIFE_PORT:-7583}"
MODES="${N2C_LIFE_MODES:-plain N2C_NC_RX_REVIVE N2C_NC_NO_TELL N2C_NC_PLAIN_CMP}"
export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
export PKERNEL_GALAXY=0

make -C "$ROOT/relay" > "$WORK/relay-build.log" 2>&1 || { echo "[n2c-life] relay build FAIL"; tail -20 "$WORK/relay-build.log"; exit 1; }
cp "$ROOT/relay/relay" "$WORK/relay"

build() {  # <tag> <cflags>: leaves $WORK/<tag>/p-kernel
    mkdir -p "$WORK/$1"
    make -C "$BOOT" clean >/dev/null 2>&1
    make -C "$BOOT" EXTRA_CFLAGS="$2" > "$WORK/$1/build.log" 2>&1 || return 1
    cp "$BOOT/p-kernel" "$WORK/$1/p-kernel"
}
waitfor() {  # <file> <ERE> <secs>: prints the seconds it took or TIMEOUT
    local n=0
    while [ "$n" -lt "$(( $3 * 10 ))" ]; do
        if grep -aqE "$2" "$1" 2>/dev/null; then echo "$(( n / 10 )).$(( n % 10 ))"; return 0; fi
        sleep 0.1; n=$((n + 1))
    done
    echo TIMEOUT; return 1
}
waitmore() {  # <file> <ERE> <count before> <secs>: seconds until one more match, or TIMEOUT
    local n=0
    while [ "$n" -lt "$(( $4 * 10 ))" ]; do
        [ "$(grep -acE "$2" "$1")" -gt "$3" ] && { echo "$(( n / 10 )).$(( n % 10 ))"; return 0; }
        sleep 0.1; n=$((n + 1))
    done
    echo TIMEOUT; return 1
}
node() {  # <bin> <dir> <id> <log> <fifo>: sets LASTPID
    mkfifo "$5" 2>/dev/null
    env PKERNEL_NODE_ID="$3" PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$2" PKERNEL_RELAY="127.0.0.1:$PORT" \
        "$1" > "$4" 2>&1 < "$5" &
    LASTPID=$!; PIDS="$PIDS $LASTPID"
}
DEAD_RE='\[swim\] (gossip: )?node 1 -> DEAD'
UP_RE='\[swim\] node 1 (discovered|recovered)|gossip: node 1 discovered'

unit() {  # <tag>: I7
    local bin="$WORK/$1/p-kernel" log="$WORK/$1/life.log"
    printf 'nodes life\nexit\n' | timeout 60 env PKERNEL_PFS_DIR="$WORK/$1/u" "$bin" > "$log" 2>&1
    grep -a '^\[swim-life\] RED' "$log" | tr -d '\r' | sed "s/^/[n2c-life] $1 I7 detail: /"
    grep -aq '^\[swim-life\] PASS' "$log"
}

i8() {  # <tag>: I8
    local T="$WORK/$1/i8" bin="$WORK/$1/p-kernel" k m ok=1 P1 P2 P3 RL
    rm -rf "$T"; mkdir -p "$T/d1" "$T/d2" "$T/d3"
    "$WORK/relay" -p "$PORT" > "$T/relay.log" 2>&1 & RL=$!; PIDS="$PIDS $RL"
    sleep 1
    node "$bin" "$T/d1" 1 "$T/n1.log" "$T/f1"; P1=$LASTPID; exec 7<>"$T/f1"
    node "$bin" "$T/d2" 2 "$T/n2.log" "$T/f2"; P2=$LASTPID; exec 8<>"$T/f2"
    node "$bin" "$T/d3" 3 "$T/n3.log" "$T/f3"; P3=$LASTPID; exec 9<>"$T/f3"
    echo "[n2c-life] $1 I8 up: n1 $(waitfor "$T/n1.log" "$UP_RE" 120) s, n3 $(waitfor "$T/n3.log" "$UP_RE" 120) s"
    sleep 10
    kill -STOP "$P2"
    echo "[n2c-life] $1 I8 DEAD: n1 $(waitfor "$T/n1.log" "$DEAD_RE" 180) s, n3 $(waitfor "$T/n3.log" "$DEAD_RE" 180) s"
    sleep 20
    for k in 1 3; do wc -l < "$T/n$k.log" > "$T/mark$k"; done
    kill -CONT "$P2"
    sleep 60
    for k in 1 3; do
        m=$(cat "$T/mark$k")
        tail -n +"$((m + 1))" "$T/n$k.log" | tr -d '\r' > "$T/after$k"
        local rx probe back
        rx=$(grep -c 'node 1 recovered' "$T/after$k")
        probe=$(grep -cE 'node 1 -> ALIVE \((direct|indirect) probe\)' "$T/after$k")
        back=$(grep -c 'node 1 back at incarnation' "$T/after$k")
        echo "[n2c-life] $1 I8 n$k after CONT: rx=$rx probe=$probe back=$back $(grep -m1 'node 1 back at' "$T/after$k")"
        { [ "$back" -ge 1 ] && [ "$rx" = 0 ] && [ "$probe" = 0 ]; } || ok=0
    done
    kill -9 "$P1" "$P2" "$P3" "$RL" 2>/dev/null
    exec 7>&- 8>&- 9>&-
    wait 2>/dev/null
    [ "$ok" = 1 ]
}

i5b() {  # <tag>: I5b on the build <tag>
    local T="$WORK/$1/i5b" bin="$WORK/$1/p-kernel" ok=1 P1 P2 P3 RL c1 c3 b1 b3
    local BACK_RE='node 1 recovered|node 1 -> ALIVE|node 1 back at incarnation'
    rm -rf "$T"; mkdir -p "$T/d1" "$T/d2" "$T/d3"
    "$WORK/relay" -p "$PORT" > "$T/relay.log" 2>&1 & RL=$!; PIDS="$PIDS $RL"
    sleep 1
    node "$bin" "$T/d1" 1 "$T/n1.log" "$T/f1"; P1=$LASTPID; exec 7<>"$T/f1"
    node "$bin" "$T/d2" 2 "$T/n2a.log" "$T/f2a"; P2=$LASTPID; exec 8<>"$T/f2a"
    node "$bin" "$T/d3" 3 "$T/n3.log" "$T/f3"; P3=$LASTPID; exec 9<>"$T/f3"
    echo "[n2c-life] $1 I5b up: n1 $(waitfor "$T/n1.log" "$UP_RE" 120) s, n3 $(waitfor "$T/n3.log" "$UP_RE" 120) s"
    sleep 10
    kill -9 "$P2"
    echo "[n2c-life] $1 I5b DEAD: n1 $(waitfor "$T/n1.log" "$DEAD_RE" 180) s, n3 $(waitfor "$T/n3.log" "$DEAD_RE" 180) s"
    c1=$(grep -acE "$BACK_RE" "$T/n1.log"); c3=$(grep -acE "$BACK_RE" "$T/n3.log")
    sleep 20
    exec 8>&-
    node "$bin" "$T/d2" 2 "$T/n2b.log" "$T/f2b"; P2=$LASTPID; exec 8<>"$T/f2b"
    b1=$(waitmore "$T/n1.log" "$BACK_RE" "$c1" 60); b3=$(waitmore "$T/n3.log" "$BACK_RE" "$c3" 60)
    echo "[n2c-life] $1 I5b back after the restart: n1 $b1 s, n3 $b3 s; n2b $(grep -a '^\[swim\] incarnation' "$T/n2b.log" | tr -d '\r') SELF-SUSPICION $(grep -ac 'SELF-SUSPICION' "$T/n2b.log")"
    grep -aE 'node 1 (back at|recovered|-> ALIVE)' "$T/n3.log" | tail -1 | tr -d '\r' | sed "s/^/[n2c-life] $1 I5b n3 last: /"
    [ "$b1" = TIMEOUT ] && ok=0
    [ "$b3" = TIMEOUT ] && ok=0
    kill -9 "$P1" "$P2" "$P3" "$RL" 2>/dev/null
    exec 7>&- 8>&- 9>&-
    wait 2>/dev/null
    [ "$ok" = 1 ]
}

FAIL=0
r() {  # <tag> <check> <0|1 passed>
    if [ "$3" = 1 ]; then echo "[n2c-life] $1 $2 PASS"; else echo "[n2c-life] $1 $2 FAIL"; REDS="$REDS $2"; fi
}
for m in $MODES; do
    REDS=""
    # items: the `nodes life` items that must be RED under the falsifier
    case "$m" in
        plain)            cf="";     checks="I7 I8 I5b I5bR"; want="";        items="" ;;
        N2C_NC_RX_REVIVE) cf="-D$m"; checks="I7 I8";          want="I7 I8";   items="a1s a2s cp ch" ;;
        N2C_NC_NO_TELL)   cf="-D$m"; checks="I7 I5bR";        want="I7 I5bR"; items="a1q a2q" ;;
        N2C_NC_PLAIN_CMP) cf="-D$m"; checks="I7";             want="I7";      items="d e fb" ;;
        *) echo "[n2c-life] unknown mode $m"; FAIL=1; continue ;;
    esac
    if ! build "$m" "$cf"; then echo "[n2c-life] $m build FAIL"; tail -20 "$WORK/$m/build.log"; FAIL=1; continue; fi
    for c in $checks; do
        case "$c" in
            I7)   unit "$m" && r "$m" I7 1 || r "$m" I7 0 ;;
            I8)   i8 "$m"   && r "$m" I8 1 || r "$m" I8 0 ;;
            I5b)  i5b "$m"  && r "$m" I5b 1 || r "$m" I5b 0 ;;
            I5bR)
                if ! build "$m-R" "$cf -DN2C_NC_INC_RESET"; then echo "[n2c-life] $m-R build FAIL"; FAIL=1; continue; fi
                i5b "$m-R" && r "$m" I5bR 1 || r "$m" I5bR 0 ;;
        esac
    done
    if [ "$m" = plain ]; then
        [ -z "$REDS" ] || { echo "[n2c-life] plain red:$REDS"; FAIL=1; }
    else
        for w in $want; do
            case " $REDS " in
                *" $w "*) echo "[n2c-life] $m turns $w red (as it must)" ;;
                *) echo "[n2c-life] $m does NOT turn $w red"; FAIL=1 ;;
            esac
        done
        for it in $items; do
            if grep -aq "^\[swim-life\] RED  $it " "$WORK/$m/life.log"; then
                echo "[n2c-life] $m turns I7 $it red (as it must)"
            else
                echo "[n2c-life] $m does NOT turn I7 $it red"; FAIL=1
            fi
        done
    fi
done
if [ "$FAIL" = 0 ]; then echo "[n2c-life] ALL PASS"; exit 0; fi
echo "[n2c-life] RED"; exit 1
