#!/bin/bash
# ---------------------------------------------------------------------------
# Relay HA: kill the relay SPOF (structural gap #3 from PR #4).
#
# Two relays (ports 7400 / 7401) + three p-kernel nodes configured with
#   PKERNEL_RELAY="127.0.0.1:7400,127.0.0.1:7401"
# All nodes hold the SAME ordered list and follow one deterministic rule:
# "use the first relay on the list that is alive" (see
# docs/architecture/relay-ha.md). This script asserts the full story:
#
#   phase 1  mesh forms on relay#1 (7400); kdemo heartbeats flow A<->B<->C
#   phase 2  relay#1 is SIGKILLed -> within 15 s every node fails over to
#            relay#2 (7401), re-registers, and kdds pub/sub (kdemo) resumes
#   phase 3  relay#1 is restarted -> within 25 s (10 s failback probe period
#            + failover margin) every node returns to relay#1 and kdds
#            pub/sub flows through it again
#
# Exit code 0 only if every assertion holds.
#
# Usage:   ./run_relay_failover.sh
# Watch:   /tmp/pkha_node{1,2,3}.log /tmp/pkha_relay{1,2,1b}.log
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "unsupported host arch $(uname -m)"; exit 1 ;;
esac

[ -x "$BOOT/p-kernel" ]    || make -C "$BOOT"        >/dev/null || exit 1
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay"  >/dev/null || exit 1

export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
export PKERNEL_RELAY="127.0.0.1:7400,127.0.0.1:7401"

PORT1=7400
PORT2=7401

PIDS=()                       # killed by PID at exit — never pkill
cleanup() {
    for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
}
trap cleanup EXIT

ts() { date '+%H:%M:%S'; }
note() { echo "[$(ts)] $*"; }
fail() { echo "[$(ts)] FAIL: $*"; exit 1; }

# wait_for <timeout_s> <what> <file> <pattern> [min_count]
wait_for() {
    local tmo=$1 what=$2 file=$3 pat=$4 min=${5:-1}
    local waited=0
    while [ "$waited" -lt $((tmo * 2)) ]; do
        if [ "$(grep -c -- "$pat" "$file" 2>/dev/null)" -ge "$min" ]; then
            note "OK: $what"
            return 0
        fi
        sleep 0.5
        waited=$((waited + 1))
    done
    echo "----- tail $file -----"; tail -5 "$file" 2>/dev/null
    fail "$what (timeout ${tmo}s waiting for '$pat' x$min in $file)"
}

# kdemo publishes with the DRPC node id, which is PKERNEL_NODE_ID - 1
# (cmd_net: nid = mac[5]-1). So node2 shows up as "n1", node3 as "n2".
rx_count() {  # kdemo heartbeats received by node1 from kdemo id $1
    grep -c "\[kdemo-rx\] n$1 " /tmp/pkha_node1.log 2>/dev/null || true
}

# --- phase 0: start everything -------------------------------------------
note "starting relay#1 on :$PORT1 and relay#2 on :$PORT2"
"$ROOT/relay/relay" -p $PORT1 -v >/tmp/pkha_relay1.log 2>&1 & RELAY1=$!; PIDS+=($RELAY1)
"$ROOT/relay/relay" -p $PORT2 -v >/tmp/pkha_relay2.log 2>&1 & RELAY2=$!; PIDS+=($RELAY2)
sleep 1

note "starting nodes 1..3 (PKERNEL_RELAY=$PKERNEL_RELAY, kdemo on each)"
for i in 1 2 3; do
    { echo "kdemo"; sleep 180; } | \
        PKERNEL_NODE_ID=$i PKERNEL_AUTONET=1 "$BOOT/p-kernel" \
        >/tmp/pkha_node$i.log 2>&1 &
    PIDS+=($!)
done

# --- phase 1: mesh forms on relay#1 ---------------------------------------
note "phase 1: waiting for mesh on relay#1"
wait_for 20 "relay#1 saw all 3 nodes register"          /tmp/pkha_relay1.log "registered" 3
wait_for 20 "node1 receives kdds heartbeats from node2" /tmp/pkha_node1.log  "\[kdemo-rx\] n1 "
wait_for 20 "node1 receives kdds heartbeats from node3" /tmp/pkha_node1.log  "\[kdemo-rx\] n2 "

# --- phase 2: kill relay#1 -> failover to relay#2 --------------------------
note "phase 2: SIGKILL relay#1 (pid $RELAY1)"
kill -KILL "$RELAY1" 2>/dev/null
wait "$RELAY1" 2>/dev/null

T_KILL=$SECONDS
for i in 1 2 3; do
    wait_for 15 "node$i failed over to relay#2 (:$PORT2)" \
        /tmp/pkha_node$i.log "failover -> relay#1 127.0.0.1:$PORT2"
done
note "all 3 nodes failed over $((SECONDS - T_KILL))s after the kill"

wait_for 15 "relay#2 saw all 3 nodes register" /tmp/pkha_relay2.log "registered" 3

# kdds pub/sub must RESUME through relay#2: counts strictly increase
# from the post-failover snapshot.
C2=$(rx_count 1); C3=$(rx_count 2)
note "kdemo snapshot after failover: node2=$C2 node3=$C3 — waiting for traffic via relay#2"
wait_for 15 "kdds pub/sub node2->node1 flows via relay#2" /tmp/pkha_node1.log "\[kdemo-rx\] n1 " $((C2 + 2))
wait_for 15 "kdds pub/sub node3->node1 flows via relay#2" /tmp/pkha_node1.log "\[kdemo-rx\] n2 " $((C3 + 2))

# --- phase 3: revive relay#1 -> deterministic failback ---------------------
note "phase 3: restarting relay#1 on :$PORT1"
"$ROOT/relay/relay" -p $PORT1 -v >/tmp/pkha_relay1b.log 2>&1 & RELAY1B=$!; PIDS+=($RELAY1B)

T_BACK=$SECONDS
for i in 1 2 3; do
    wait_for 25 "node$i failed back to relay#1 (:$PORT1)" \
        /tmp/pkha_node$i.log "failback -> relay#0 127.0.0.1:$PORT1"
done
note "all 3 nodes failed back $((SECONDS - T_BACK))s after relay#1 revived"

wait_for 15 "revived relay#1 saw all 3 nodes register" /tmp/pkha_relay1b.log "registered" 3

# kdds pub/sub must flow through relay#1 again.
C2=$(rx_count 1); C3=$(rx_count 2)
wait_for 15 "kdds pub/sub node2->node1 flows via revived relay#1" /tmp/pkha_node1.log "\[kdemo-rx\] n1 " $((C2 + 2))
wait_for 15 "kdds pub/sub node3->node1 flows via revived relay#1" /tmp/pkha_node1.log "\[kdemo-rx\] n2 " $((C3 + 2))

echo
note "PASS — relay#1 death and revival were both survived; all nodes converged"
echo "        logs: /tmp/pkha_node{1,2,3}.log /tmp/pkha_relay{1,2,1b}.log"
exit 0
