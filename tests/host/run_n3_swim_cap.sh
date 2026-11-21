#!/bin/bash
# tests/host/run_n3_swim_cap.sh — [n3-swim-cap]: gate S5 of N3 step 1, the SWIM
# tables sized by the region (docs/architecture/30-module/research/n3-swim-region.md).
# Written before the implementation (test first): on master e4ca62c8 C1 and C2
# are RED (there is no slot table and no "[swim] slots" line).
#
# One hosted build with -DSWIM_PEERS=<P> (default 8), swim.o with -fsanitize=bounds (trap),
# N (default 12) narrow nodes (ids 1..N) on one relay, so every node hears of
# N-1 > P peers. After the wait, `nodes` on every node, then:
#   C1  every node prints "[swim] slots U/C refused F" with C == P and P-1 <= U <= P
#       (audit-205: U >= P-1 added before the implementation. Without it a node
#        that refuses everyone (U 0, F > 0) passed C1..C5: C3 is filled by the
#        drpc heartbeat, not by SWIM. With N-1 > P live peers the slots fill;
#        P-1 leaves room for one DEAD slot being handed over)
#   C2  every node has F > 0 (it was told about more peers than it has slots)
#   C3  every node's [cluster] table shows >= P-2 ALIVE peers
#       (the drpc heartbeat fills the table for all, step 2 of N3; this only
#        checks membership did not collapse)
#   C4  every node is alive at the end (built with -fsanitize=bounds and
#       trap-on-error: a table indexed past its end kills the node)
#   C5  every node answered the last `nodes` (printed the [cluster] header)
#   C6  (H41 (b), 11-21) node N is killed; every node that held its slot says
#       DEAD about it through SWIM within N3C_C6_WAIT (60) s (details below)
# Negative controls: N3C_CFLAGS=-DN3_NC_NO_CAP (the implementation's switch
# that takes the cap away: a node keeps all N-1) => C1 RED;
# -DN3_NC_NO_PROBE_SLOTS (the slots are never probed) => C6 RED.
# Env: N3C_N (12), N3C_PEERS (8), N3C_PORT (7611), N3C_WAIT (60 s),
#      N3C_CFLAGS (extra), N3C_WORK (keep the logs there).
# Output: "[n3-swim-cap] C<n> PASS|FAIL ...", then ALL PASS (exit 0) or RED (exit 1).
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
N="${N3C_N:-12}"; P="${N3C_PEERS:-8}"; PORT="${N3C_PORT:-7611}"; WAIT="${N3C_WAIT:-60}"
CFX="${N3C_CFLAGS:-}"
REL="$ROOT/relay/relay"
if [ -n "${N3C_WORK:-}" ]; then WORK="$N3C_WORK"; mkdir -p "$WORK"; else WORK="$(mktemp -d /tmp/n3cap.XXXXXX)"; fi
LOG="$WORK/logs"; rm -rf "$LOG"; mkdir -p "$LOG"
declare -A PID
RH=0; REDS=""
cleanup() {
    for i in "${!PID[@]}"; do kill -9 "${PID[$i]}" 2>/dev/null; eval "exec $((i + 9))>&-" 2>/dev/null; done
    [ "$RH" != 0 ] && kill -9 "$RH" 2>/dev/null
    wait 2>/dev/null
    make -C "$BOOT" clean > /dev/null 2>&1
    rm -rf "$WORK"/d[0-9]* "$WORK"/f[0-9]*
    [ -z "${N3C_WORK:-}" ] && rm -rf "$WORK"
}
trap cleanup EXIT
r() { echo "[n3-swim-cap] $1 $2 ${3:-}"; [ "$2" = FAIL ] && REDS="$REDS $1"; return 0; }
cnt() { local c; c=$(grep -ac -- "$2" "$1" 2>/dev/null); echo "${c:-0}"; }

make -C "$ROOT/relay" > "$WORK/relay-build.log" 2>&1 || { echo "[n3-swim-cap] relay build FAIL"; exit 2; }
make -C "$BOOT" clean > /dev/null 2>&1
# bounds with trap-on-error, on swim.o ONLY: the hosted link is -static (no libubsan), so
# an index past a table's end stops the node at once (ud2 = SIGILL); C4 sees it.
# Not the whole build: mtkernel3's ready_queue.h:117 reads tskque[NUM_TSKPRI] when the
# queue is empty (the T-Kernel sentinel, one past the array onto the next member) and
# every node trapped there at boot (11-21, gdb: knl_make_non_ready <- tk_ext_tsk).
SAN="-fsanitize=bounds -fsanitize-undefined-trap-on-error"
if ! make -C "$BOOT" EXTRA_CFLAGS="-DSWIM_PEERS=$P $CFX" > "$WORK/build.log" 2>&1 \
   || ! rm -f "$BOOT/swim.o" \
   || ! make -C "$BOOT" swim.o EXTRA_CFLAGS="-DSWIM_PEERS=$P $SAN $CFX" >> "$WORK/build.log" 2>&1 \
   || ! make -C "$BOOT" EXTRA_CFLAGS="-DSWIM_PEERS=$P $CFX" >> "$WORK/build.log" 2>&1 \
   || ! grep -aq 'swim.c' "$WORK/build.log" || ! grep -a 'swim.c' "$WORK/build.log" | grep -aq -- '-fsanitize=bounds'; then
    echo "[n3-swim-cap] build FAIL (or swim.o was not built with $SAN)"; tail -20 "$WORK/build.log"; exit 2
fi
echo "[n3-swim-cap] N=$N SWIM_PEERS=$P extra='$CFX'"

"$REL" -p "$PORT" > "$LOG/relay.log" 2>&1 &
RH=$!
sleep 1
for i in $(seq 1 "$N"); do
    fifo="$WORK/f$i"; rm -f "$fifo"; mkfifo "$fifo"; mkdir -p "$WORK/d$i"
    env PKERNEL_NODE_ID="$i" PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$WORK/d$i" PKERNEL_RELAY="127.0.0.1:$PORT" \
        "$BOOT/p-kernel" < "$fifo" > "$LOG/node$i.log" 2>&1 &
    PID[$i]=$!
    eval "exec $((i + 9))<>\"$fifo\""
done
# up: every node printed "mind_net_task up" (120 s at most)
t=0
while [ "$t" -lt 240 ]; do
    up=0
    for i in $(seq 1 "$N"); do [ "$(cnt "$LOG/node$i.log" 'mind_net_task up')" -ge 1 ] && up=$((up + 1)); done
    [ "$up" -eq "$N" ] && break
    sleep 0.5; t=$((t + 1))
done
echo "[n3-swim-cap] up $up/$N after $((t / 2)) s; waiting $WAIT s"
for i in $(seq 1 "$N"); do printf '%s\n' "stop now" >&$((i + 9)); done
sleep "$WAIT"
for i in $(seq 1 "$N"); do printf '%s\n' "nodes" >&$((i + 9)); done
sleep 5
dead=""
for i in $(seq 1 "$N"); do kill -0 "${PID[$i]}" 2>/dev/null || dead="$dead $i"; done

c1=0; c2=0; c3=0; c5=0; rec=""
for i in $(seq 1 "$N"); do
    L="$LOG/node$i.log"
    line=$(tr -d '\r' < "$L" | grep -a '^\[swim\] slots ' | tail -1)
    u=$(echo "$line" | sed -n 's/^\[swim\] slots \([0-9]*\)\/\([0-9]*\) refused \([0-9]*\).*/\1/p')
    cc=$(echo "$line" | sed -n 's/^\[swim\] slots \([0-9]*\)\/\([0-9]*\) refused \([0-9]*\).*/\2/p')
    f=$(echo "$line" | sed -n 's/^\[swim\] slots \([0-9]*\)\/\([0-9]*\) refused \([0-9]*\).*/\3/p')
    alive=$(tr -d '\r' < "$L" | awk '/^\[cluster\]  ID/{a=0; next} /^ +[0-9]+  ALIVE/{a++} END{print a+0}')
    rec="$rec $i:${u:-?}/${cc:-?}/${f:-?}/a$alive"
    [ -n "$u" ] && [ "$cc" = "$P" ] && [ "$u" -le "$P" ] && [ "$u" -ge $((P - 1)) ] && c1=$((c1 + 1))
    [ -n "$f" ] && [ "$f" -gt 0 ] && c2=$((c2 + 1))
    [ "$alive" -ge $((P - 2)) ] && c3=$((c3 + 1))
    grep -aq '^\[cluster\]  ID' "$L" && c5=$((c5 + 1))   # only the last `nodes` prints it
done
echo "[n3-swim-cap] rec node:U/C/F/alive$rec"

# C6 (H41 (b), added 11-21 13:05 run with the implementation): kill node N
# (drpc id N-1; PKERNEL_NODE_ID i is id i-1) and wait C6_WAIT s. Every other node
# whose last slots line ("... ids a b c") held id N-1 prints a SWIM line that
# it is DEAD (its own probe "[swim] node X -> DEAD" or the rumour "[swim] gossip:
# node X -> DEAD") after the kill; at least one node held it. Measured before
# writing C6 (pk-scratch/n3s/h41.sh): all 11 nodes had it DEAD through SWIM,
# drpc printed nothing about it. Negative -DN3_NC_NO_PROBE_SLOTS (the slots are
# never probed) => C6 RED.
VID=$((N - 1)); C6W="${N3C_C6_WAIT:-60}"
declare -A L0
for i in $(seq 1 $((N - 1))); do L0[$i]=$(wc -l < "$LOG/node$i.log"); done
kill -9 "${PID[$N]}" 2>/dev/null; unset "PID[$N]"
sleep "$C6W"
held=0; saw=0; miss=""
for i in $(seq 1 $((N - 1))); do
    ids=$(tr -d '\r' < "$LOG/node$i.log" | head -n "${L0[$i]}" | grep -a '^\[swim\] slots ' | tail -1 | sed -n 's/.* ids \(.*\)$/ \1 /p')
    case "$ids" in *" $VID "*) ;; *) continue ;; esac
    held=$((held + 1))
    if tail -n +"$((L0[$i] + 1))" "$LOG/node$i.log" | tr -d '\r' \
       | grep -aqE "\[swim\] (gossip: )?node $VID -> DEAD"; then saw=$((saw + 1)); else miss="$miss $i"; fi
done
echo "[n3-swim-cap] C6 killed id $VID: $held nodes held its slot, $saw of them say DEAD through SWIM"
[ "$held" -ge 1 ] && [ "$saw" -eq "$held" ] && r C6 PASS "($saw/$held slot holders: DEAD within ${C6W} s)" \
    || r C6 FAIL "($saw/$held slot holders said DEAD; not:$miss)"
[ "$c1" -eq "$N" ] && r C1 PASS "($N/$N nodes $((P - 1))<=U<=$P, C=$P)" || r C1 FAIL "($c1/$N nodes with a [swim] slots line, C=$P and $((P - 1))<=U<=$P)"
[ "$c2" -eq "$N" ] && r C2 PASS "($N/$N refused > 0)" || r C2 FAIL "($c2/$N nodes refused > 0)"
[ "$c3" -eq "$N" ] && r C3 PASS "($N/$N >= $((P - 2)) ALIVE)" || r C3 FAIL "($c3/$N nodes >= $((P - 2)) ALIVE)"
[ -z "$dead" ] && r C4 PASS "(all $N alive: no bounds trap, no crash)" || r C4 FAIL "(dead:$dead; a bounds trap or a crash, see node<i>.log)"
[ "$c5" -eq "$N" ] && r C5 PASS "($N/$N answered the last nodes)" || r C5 FAIL "($c5/$N answered the last nodes)"
if [ -z "$REDS" ]; then echo "[n3-swim-cap] ALL PASS"; exit 0; fi
echo "[n3-swim-cap] RED:$REDS"; exit 1
