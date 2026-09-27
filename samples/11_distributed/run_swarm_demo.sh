#!/bin/bash
# ---------------------------------------------------------------------------
# run_swarm_demo.sh — "turn nodes off one by one; the swarm still remembers"
# (inbox #13-1). A human-readable demo, meant to be watched or recorded
# (asciinema rec -c ./run_swarm_demo.sh), that is ALSO a check (exit 0/1).
#
# HONEST SCOPE: this is ONE machine running N p-kernel PROCESSES on one local
# relay. It is not N phones. What it shows is the software behaviour: memory
# (a small trained classifier, `dtr`, 635 parameters) spreads from the node
# that learned it to the others through p-fs, survives the death of the
# node that learned it, and is handed on to NEW nodes as long as at least one
# holder is alive. It is NOT the language brain (student), and nothing is
# written to disk (no PKERNEL_PFS_DIR) — so when every holder dies, the memory
# is gone. The last act shows exactly that (negative control).
#
# Acts:
#   1. N nodes boot. Node 1 learns (`dtr train`), saves the weights as the
#      p-fs object dtr/weights; every other node pulls it (`dtr load`).
#   2. Kill node 1 (the teacher) first, then one node at a time until ONE is
#      left. After each kill: processes alive, what a survivor's SWIM view
#      says, and a survivor's held-out accuracy.
#   3. A brand-new node (fresh id, untrained) joins and pulls the memory from
#      the last survivor. Then the last original node is killed: the memory
#      now lives only in a node that never met the teacher.
#   4. Negative control: kill that one too, start another fresh node. Nobody
#      holds the weights, so it must stay untrained.
#
# Checks (exit 1 if any fails): every node reaches ACC_MIN after act 1;
# after every kill the survivor stays >= ACC_MIN; the newcomer goes from
# < ACC_MIN to >= ACC_MIN; the act-4 node stays < ACC_MIN.
#
# Usage:  ./run_swarm_demo.sh            (N=10)
#         N=6 ./run_swarm_demo.sh
# Tunables: N (3..30), PORT (27810), SETTLE (12s), ACC_MIN (90.0)
# Logs:   /tmp/pksd_node<id>.log /tmp/pksd_relay.log
# ---------------------------------------------------------------------------
set -u

N="${N:-10}"
PORT="${PORT:-27810}"
SETTLE="${SETTLE:-12}"
ACC_MIN="${ACC_MIN:-90.0}"

if [ "$N" -lt 3 ] || [ "$N" -gt 30 ]; then
    echo "N must be in 3..30 (two fresh ids are added, DNODE_MAX=32); got N=$N" >&2; exit 2
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "unsupported host arch $(uname -m)"; exit 1 ;;
esac
[ -x "$BOOT/p-kernel" ]    || make -C "$BOOT"       >/dev/null || exit 1
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay" >/dev/null || exit 1

export PKERNEL_RELAY_KEY=5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d
export PKERNEL_RELAY_HOST=127.0.0.1
export PKERNEL_RELAY_PORT="$PORT"
export PKERNEL_GALAXY=0
unset PKERNEL_RTT_ZONE_SIZE PKERNEL_RTT_ZONE_PENALTY PKERNEL_PFS_DIR 2>/dev/null || true

LOGPFX=/tmp/pksd
FIFODIR="$(mktemp -d /tmp/pksd_fifo.XXXXXX)"
rm -f "${LOGPFX}_node"*.log "${LOGPFX}_relay.log" 2>/dev/null

RELAY_PID=
declare -a NODE_PID
MAXID=$((N + 2))

cleanup() {
    # kill by explicit PID only (never pkill -f)
    for id in $(seq 1 "$MAXID"); do
        p="${NODE_PID[$id]:-}"
        [ -n "$p" ] && kill -9 "$p" 2>/dev/null
    done
    [ -n "$RELAY_PID" ] && kill "$RELAY_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$FIFODIR" 2>/dev/null
}
trap cleanup EXIT

nodelog() { echo "${LOGPFX}_node$1.log"; }
start_node() {                  # start_node <id>
    local id="$1" fifo="$FIFODIR/in$1" fd=$((100 + $1))
    [ -p "$fifo" ] || mkfifo "$fifo"
    eval "exec $fd<>'$fifo'"    # hold the FIFO open: no EOF between commands
    PKERNEL_NODE_ID=$id PKERNEL_AUTONET=1 "$BOOT/p-kernel" \
        <"$fifo" >>"$(nodelog "$id")" 2>&1 &
    NODE_PID[$id]=$!
    disown
}
kill_node() { kill -9 "${NODE_PID[$1]}" 2>/dev/null; NODE_PID[$1]=""; }
node_running() { [ -n "${NODE_PID[$1]:-}" ] && kill -0 "${NODE_PID[$1]}" 2>/dev/null; }
send() { local id="$1"; shift; eval "printf '%s\n' \"\$*\" >&$((100 + id))"; }
mark() { wc -l <"$(nodelog "$1")"; }
slice() { tail -n "+$(( $2 + 1 ))" "$(nodelog "$1")" 2>/dev/null; }
wait_log() {                    # wait_log <id> <from_mark> <regex> <timeout_s>
    local i=0
    while [ "$i" -lt $(( $4 * 2 )) ]; do
        slice "$1" "$2" | grep -qE "$3" && return 0
        sleep 0.5; i=$((i + 1))
    done
    return 1
}
get_acc() {                     # held-out accuracy of <id>, or ""
    local id="$1" t M acc
    for t in 1 2 3; do
        M=$(mark "$id"); send "$id" "dtr eval"
        if wait_log "$id" "$M" '\[dtr\] eval held-out' 15; then
            acc=$(slice "$id" "$M" | grep -E '\[dtr\] eval held-out' | tail -1 \
                  | grep -oE 'acc [0-9.]+' | awk '{print $2}')
            [ -n "$acc" ] && { echo "$acc"; return 0; }
        fi
    done
    echo ""
}
load_weights() {                # load_weights <id> <tries> -> 0 if loaded
    local id="$1" tries="$2" t M
    for t in $(seq 1 "$tries"); do
        M=$(mark "$id"); send "$id" "dtr load"
        wait_log "$id" "$M" "weights loaded from p-fs object" 4 && return 0
        sleep 2
    done
    return 1
}
swim_dead() {                   # how many DEAD <id>'s SWIM view shows
    local M S
    M=$(mark "$1"); send "$1" "nodes"; sleep 2
    S=$(slice "$1" "$M")
    printf '%s\n' "$S" | grep -cE '^ +[0-9]+ +DEAD'
}
ge() { awk -v a="${1:-0}" -v b="$2" 'BEGIN { exit !(a+0 >= b+0) }'; }
lt() { awk -v a="${1:-0}" -v b="$2" 'BEGIN { exit !(a+0 <  b+0) }'; }
alive_count() { local c=0 id; for id in $(seq 1 "$MAXID"); do node_running "$id" && c=$((c+1)); done; echo "$c"; }

FAIL=0
ok()  { echo "   ok    $1"; }
bad() { echo "   FAIL  $1"; FAIL=$((FAIL + 1)); }
say() { echo; echo "== $* =="; }

echo "p-kernel swarm demo — ONE machine, $N p-kernel processes, one local relay."
echo "(Not $N phones. The memory is a 635-parameter classifier, kept in RAM only.)"

say "Act 1: $N nodes boot; node 1 learns, the others pull what it learned"
"$ROOT/relay/relay" -p "$PORT" -v >"${LOGPFX}_relay.log" 2>&1 & RELAY_PID=$!
disown; sleep 1
for id in $(seq 1 "$N"); do start_node "$id"; sleep 0.15; done
sleep "$SETTLE"
echo "   alive processes: $(alive_count)"

B=$(get_acc 1)
echo "   node 1 before learning: held-out accuracy ${B:-?}%"
M=$(mark 1); send 1 "dtr train"
wait_log 1 "$M" '\[dtr\] trained [0-9]+ epochs' 120 || bad "node 1 did not finish training"
A1=$(get_acc 1)
echo "   node 1 after learning:  held-out accuracy ${A1:-?}%"
M=$(mark 1); send 1 "dtr save"
wait_log 1 "$M" "saved as p-fs object" 30 || bad "node 1 'dtr save' not confirmed"
for id in $(seq 2 "$N"); do
    if load_weights "$id" 20; then
        a=$(get_acc "$id")
        if ge "$a" "$ACC_MIN"; then ok "node $id pulled the memory: ${a}%"
        else bad "node $id loaded but acc ${a:-?}% < ${ACC_MIN}%"; fi
    else
        bad "node $id never received the memory"
    fi
done

say "Act 2: turn nodes off one by one — the teacher (node 1) goes first"
KILLED=0
for v in $(seq 1 $((N - 1))); do
    kill_node "$v"; KILLED=$((KILLED + 1))
    w=$((v + 1))                              # lowest-id survivor answers
    D=0; t=0
    while [ "$t" -lt 10 ]; do                 # give SWIM up to ~40s to notice
        D=$(swim_dead "$w"); [ "$D" -ge "$KILLED" ] && break
        sleep 2; t=$((t + 1))
    done
    a=$(get_acc "$w")
    line="killed node $v -> alive $(alive_count)/$N; node $w's SWIM sees $D dead; node $w answers ${a:-?}%"
    if ge "$a" "$ACC_MIN"; then ok "$line"; else bad "$line"; fi
done

say "Act 3: a brand-new node joins and learns from the last survivor (node $N)"
NEW=$((N + 1))
start_node "$NEW"; sleep "$SETTLE"
b=$(get_acc "$NEW")
echo "   node $NEW before pulling: ${b:-?}% (untrained)"
lt "$b" "$ACC_MIN" || bad "node $NEW was already trained before pulling (${b}%)"
if load_weights "$NEW" 20; then
    a=$(get_acc "$NEW")
    if ge "$a" "$ACC_MIN"; then ok "node $NEW inherited the memory: ${a}%"
    else bad "node $NEW loaded but acc ${a:-?}% < ${ACC_MIN}%"; fi
else
    bad "node $NEW could not get the memory from node $N"
fi
kill_node "$N"
a=$(get_acc "$NEW")
line="killed node $N (the last original) -> only node $NEW remains, which never met node 1: ${a:-?}%"
if ge "$a" "$ACC_MIN"; then ok "$line"; else bad "$line"; fi

say "Act 4 (negative control): when nobody holds it, the memory is gone"
kill_node "$NEW"
LAST=$((N + 2))
start_node "$LAST"; sleep "$SETTLE"
if load_weights "$LAST" 5; then
    bad "node $LAST loaded weights although every holder is dead"
fi
a=$(get_acc "$LAST")
line="all holders dead; fresh node $LAST: ${a:-?}% (stays untrained — nothing is on disk)"
if lt "$a" "$ACC_MIN"; then ok "$line"; else bad "$line"; fi

echo
if [ "$FAIL" -eq 0 ]; then echo "[swarm-demo] PASS"; exit 0
else echo "[swarm-demo] FAIL ($FAIL)"; exit 1; fi
