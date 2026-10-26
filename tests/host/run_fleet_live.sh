#!/bin/bash
# tests/host/run_fleet_live.sh — ROADMAP N1: N hosted x86_64 nodes live on one
# relay (one machine). Design, checks and negative controls fixed before the
# first run: docs/architecture/30-module/research/n1-fleet-64.md.
#
# Checks (C1..C6 are samples/11_distributed/run_Nnode_scale.sh's 6, C4P and the
# alive half of C6 only make them stronger):
#   C1  all N ids registered on the relay
#   C2  node1 `world` known nodes >= 3N/4
#   C3  node1 printed a region (size >= 1; true alone, the content is C4P)
#   C4  a dkva aggregate with KV > 0 or an `infer` class, and no "no result"
#   C4P a dkva aggregate gathered >= 1 region peer
#   C5  zero table-exhaustion lines in any log
#   C6  no crash signature, every node booted, every node alive at the end
#
# Env: FLEET_N (32, 2..64), FLEET_PORT (7591), FLEET_BATCH (8: start this many,
#      wait for their `mind_net_task up`, then the next batch), FLEET_STOP (1:
#      `stop now` on every node once it is up = no sleep work; this scenario
#      does not use learning), FLEET_CONV_MAX (300 s), FLEET_LOGDIR,
#      FLEET_ZONE (0; >0 sets PKERNEL_RTT_ZONE_SIZE = NC3, 1 => C4P red),
#      FLEET_KILL (0; >0 kill -9 that many of the last ids after the commands
#      = NC2, C6 red), FLEET_RELAY_V (1: relay -v, needed for relay_Bps_node;
#      0: quiet relay, relay_Bps_node=na). NC1 is a tree whose drpc.h and
#      arch/linux/node_id.c say DNODE_MAX 32 (both #define it without #ifndef,
#      so -DDNODE_MAX on the command line does not take), then FLEET_N=48 =>
#      C2 red. The script does not build such trees itself.
# Output: one "[fleet] rec ..." line (udp_drops: /proc/net/udp drops at the end,
#   relay socket and all sockets of the box), "[fleet] C<n> PASS|FAIL ...",
#   then "[fleet] ALL PASS" (exit 0) or "[fleet] RED: .." (exit 1).
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BOOT="$ROOT/boot/linux_x86_64"
[ -x "$BOOT/p-kernel" ]    || make -C "$BOOT"       >/dev/null || exit 1
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay" >/dev/null || exit 1

export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
unset PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN PKERNEL_RTT_ZONE_SIZE
N="${FLEET_N:-32}"; PORT="${FLEET_PORT:-7591}"; BATCH="${FLEET_BATCH:-8}"
STOP="${FLEET_STOP:-1}"; CONV_MAX="${FLEET_CONV_MAX:-300}"
ZONE="${FLEET_ZONE:-0}"; KILLN="${FLEET_KILL:-0}"; RELAY_V="${FLEET_RELAY_V:-1}"
[ "$N" -ge 2 ] && [ "$N" -le 64 ] || { echo "[fleet] FLEET_N must be 2..64"; exit 2; }
[ "$ZONE" -gt 0 ] && export PKERNEL_RTT_ZONE_SIZE="$ZONE"
WORK="$(mktemp -d /tmp/fleet.XXXXXX)"
LOGDIR="${FLEET_LOGDIR:-$WORK/logs}"; mkdir -p "$LOGDIR"
declare -A PID
RH=0; REDS=""
HZ=$(getconf CLK_TCK)

note() { echo "[$(date '+%H:%M:%S')] $*"; }
res()  { echo "[fleet] $1 $2: $3"; [ "$2" = FAIL ] && REDS="$REDS $1"; return 0; }
cleanup() {
    for i in $(seq 1 "$N"); do
        [ "${PID[$i]:-0}" != 0 ] && kill -9 "${PID[$i]}" 2>/dev/null
        eval "exec $((i + 2))>&-" 2>/dev/null
    done
    [ "$RH" != 0 ] && kill -9 "$RH" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"/d[0-9]* "$WORK"/f[0-9]*
    [ -z "${FLEET_LOGDIR:-}" ] && rm -rf "$WORK"
}
trap cleanup EXIT

start_node() {  # <i>
    local i="$1" fifo="$WORK/f$1"
    rm -f "$fifo"; mkfifo "$fifo"; mkdir -p "$WORK/d$i"
    env PKERNEL_NODE_ID="$i" PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$WORK/d$i" PKERNEL_RELAY="127.0.0.1:$PORT" \
        "$BOOT/p-kernel" < "$fifo" > "$LOGDIR/node$i.log" 2>&1 &
    PID[$i]=$!
    eval "exec $((i + 2))<>\"$fifo\""
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
cpu_ticks() {  # <pid>: utime+stime
    awk '{print $14 + $15}' "/proc/$1/stat" 2>/dev/null || echo 0
}
known_now() {
    grep -aE "known nodes:" "$LOGDIR/node1.log" | tail -1 | grep -oE "[0-9]+" | head -1
}

echo "==========================================================="
echo " [fleet] N=$N batch=$BATCH stop=$STOP zone=$ZONE kill=$KILLN  logs: $LOGDIR"
echo "==========================================================="
VFLAG=""; [ "$RELAY_V" = 1 ] && VFLAG="-v"
"$ROOT/relay/relay" -p "$PORT" $VFLAG > "$LOGDIR/relay.log" 2>&1 &
RH=$!
sleep 1
T0=$(date +%s)
i=1
while [ "$i" -le "$N" ]; do
    last=$((i + BATCH - 1)); [ "$last" -gt "$N" ] && last=$N
    for j in $(seq "$i" "$last"); do start_node "$j"; sleep 0.15; done
    for j in $(seq "$i" "$last"); do
        wait_for "$LOGDIR/node$j.log" 'mind_net_task up' 120 || note "node$j: mind_net_task not seen"
        [ "$STOP" = 1 ] && send "$j" "stop now"
    done
    i=$((last + 1))
done
TL=$(date +%s)
note "all $N started in $((TL - T0)) s"

# ---- convergence: node1 `world` every 3 s ------------------------------------
CONV=none; t=0
while [ "$t" -lt "$CONV_MAX" ]; do
    send 1 "world"; sleep 3; t=$((t + 3))
    kn=$(known_now)
    if [ -n "$kn" ] && [ "$kn" -ge $((N - 1)) ]; then CONV=$(( $(date +%s) - TL )); break; fi
done
note "convergence: ${CONV} s (known ${kn:-0})"

# ---- steady window: relay bytes, CPU per node, load --------------------------
sleep 10
declare -A C0
for j in $(seq 1 "$N"); do C0[$j]=$(cpu_ticks "${PID[$j]}"); done
L1=$(wc -l < "$LOGDIR/relay.log"); sleep 20; L2=$(wc -l < "$LOGDIR/relay.log")
CT=0; for j in $(seq 1 "$N"); do c=$(cpu_ticks "${PID[$j]}"); CT=$((CT + c - ${C0[$j]})); done
RXB=$(sed -n "$((L1 + 1)),${L2}p" "$LOGDIR/relay.log" | grep -oE "\(([0-9]+) B\)$" | grep -oE "[0-9]+" | awk '{s+=$1}END{print s+0}')
LOAD=$(cut -d' ' -f1 /proc/loadavg)
RSS=$(for j in $(seq 1 "$N"); do awk '/VmRSS/{print $2}' "/proc/${PID[$j]}/status" 2>/dev/null; done | sort -n | awk '{a[NR]=$1}END{print a[int((NR+1)/2)]+0}')

# ---- node1 drives the scale commands -------------------------------------------
send 1 "world"; sleep 3
for c in nodes region dist; do send 1 "$c"; sleep 4; done
for c in "infer 50 20 90 5" "infer 10 80 30 60"; do
    pre=$(grep -acE '\[infer\] (=> class|no result)' "$LOGDIR/node1.log")
    send 1 "$c"; n=0
    while [ "$(grep -acE '\[infer\] (=> class|no result)' "$LOGDIR/node1.log")" -le "$pre" ] && [ "$n" -lt 60 ]; do sleep 0.5; n=$((n + 1)); done
done
sleep 2

if [ "$KILLN" -gt 0 ]; then
    for j in $(seq $((N - KILLN + 1)) "$N"); do note "NC2: kill -9 node$j"; kill -9 "${PID[$j]}"; done
    sleep 2
fi

# ---- record and checks ----------------------------------------------------------
L1LOG="$LOGDIR/node1.log"
REG=$(grep -aE "registered" "$LOGDIR/relay.log" | grep -oE "node [0-9]+" | awk '{print $2}' | sort -un | wc -l)
KNOWN=$(known_now); KNOWN="${KNOWN:-0}"
RGN=$(grep -acE "^\[region\] id=" "$L1LOG")
RGN_SIZE=$(grep -aE "^\[region\] id=" "$L1LOG" | grep -oE "size=[0-9]+" | grep -oE "[0-9]+" | tail -1); RGN_SIZE="${RGN_SIZE:-0}"
AGG_NONZERO=$(grep -aE "\[dkva\] aggregated " "$L1LOG" | grep -vE "\(0 KV entries\)" | wc -l)
PEERS=$(grep -aE "\[dkva\] aggregated " "$L1LOG" | sed -nE 's/.*aggregated ([0-9]+) region peers.*/\1/p' | sort -n | tail -1); PEERS="${PEERS:-0}"
INFER_OK=$(grep -acE "\[infer\] => class" "$L1LOG")
INFER_NULL=$(grep -acE "\[infer\] no result" "$L1LOG")
EXH=$(cat "$LOGDIR"/node*.log "$LOGDIR/relay.log" | grep -acE "topic table full|handle table full|pending table full|sem create failed|create failed|sem .*fail|handle .*full|slot full")
CRASH=$(cat "$LOGDIR"/node*.log | grep -acE "pc=0x[0-9a-f]+ +addr=0x[0-9a-f]+|SIGSEGV|Segmentation|signal 11|\*\*\* stack|abort")
NOBOOT=0; DEAD=""
for j in $(seq 1 "$N"); do
    grep -aqE "\[dkva\] initialized|p-kernel>|dtr distributed" "$LOGDIR/node$j.log" || NOBOOT=$((NOBOOT + 1))
    kill -0 "${PID[$j]}" 2>/dev/null || DEAD="$DEAD node$j"
done
NDEAD=$(echo $DEAD | wc -w)
SELFSUS=$(cat "$LOGDIR"/node*.log | grep -ac "SELF-SUSPICION")
PROV=$(cat "$LOGDIR"/node*.log | grep -ac "provisional relay-udp, no mesh")
TCPN=$(cat "$LOGDIR"/node*.log | grep -ac "adopted relay-tcp")
RESPN=$(grep -al "responded to node 0 " "$LOGDIR"/node*.log | wc -l)
read -r BPSN CPUN < <(python3 -c 'import sys; rx,n,ct,hz,v=map(int,sys.argv[1:]); print("%s %.1f" % ("%.0f" % (rx/20/n) if v else "na", ct/hz/20/n*100))' "$RXB" "$N" "$CT" "$HZ" "$RELAY_V")
# UDP drops (/proc/net/udp, last column): the relay socket and every socket on the box
UDPD=$(python3 -c 'import sys
port = "%04X" % int(sys.argv[1]); r = a = 0
for ln in open("/proc/net/udp").read().splitlines()[1:]:
    f = ln.split(); d = int(f[-1]); a += d
    if f[1].endswith(":" + port): r = d
print("relay=%d,all=%d" % (r, a))' "$PORT")
echo "[fleet] rec N=$N start_s=$((TL - T0)) conv_s=$CONV known=$KNOWN region=$RGN_SIZE dkva_peers=$PEERS relay_Bps_node=$BPSN rss_kB=$RSS load=$LOAD cpu_pct_node=$CPUN selfsus=$SELFSUS provisional=$PROV relay_tcp=$TCPN answered_node1=$RESPN udp_drops=$UDPD relay_v=$RELAY_V batch=$BATCH stop=$STOP zone=$ZONE kill=$KILLN"

TH=$(( (N * 3) / 4 ))
[ "$REG" -ge "$N" ] && res C1 PASS "$REG/$N registered" || res C1 FAIL "only $REG/$N registered"
[ "$KNOWN" -ge "$TH" ] && res C2 PASS "known $KNOWN >= $TH" || res C2 FAIL "known $KNOWN < $TH"
{ [ "$RGN" -ge 1 ] && [ "$RGN_SIZE" -ge 1 ]; } && res C3 PASS "region size=$RGN_SIZE" || res C3 FAIL "no region on node1"
{ { [ "$AGG_NONZERO" -ge 1 ] || [ "$INFER_OK" -ge 1 ]; } && [ "$INFER_NULL" -eq 0 ]; } \
    && res C4 PASS "aggregates non-zero $AGG_NONZERO, infer ok $INFER_OK" \
    || res C4 FAIL "aggregates non-zero $AGG_NONZERO, infer ok $INFER_OK null $INFER_NULL"
[ "$PEERS" -ge 1 ] && res C4P PASS "dkva gathered $PEERS region peers" || res C4P FAIL "dkva gathered 0 region peers"
[ "$EXH" -eq 0 ] && res C5 PASS "0 exhaustion lines" || res C5 FAIL "$EXH exhaustion lines"
{ [ "$CRASH" -eq 0 ] && [ "$NOBOOT" -eq 0 ] && [ "$NDEAD" -eq 0 ]; } \
    && res C6 PASS "no crash, $N booted, $N alive" \
    || res C6 FAIL "crash $CRASH, not booted $NOBOOT, dead:${DEAD:- none}"
if [ -z "$REDS" ]; then echo "[fleet] ALL PASS"; exit 0; fi
echo "[fleet] RED:$REDS"
exit 1
