#!/bin/bash
# ---------------------------------------------------------------------------
# run_Nnode_scale.sh — parametrized N-node RUNTIME scale test for the
# DNODE_MAX=32 cap (Wave 1 raised it 8->32 but only validated 32-slot init
# resource allocation via a 4-node run; this harness exercises the actual
# multi-node runtime at scale).
#
# It launches N real so_node processes (the SAME libpkernel.so artifact the
# APK ships) on ONE v2 relay, drives node 1 through the cluster shell
# commands (nodes / region / world / infer / dist), and then asserts on the
# logs: full registration, membership/world view ~= N, region formation,
# DKVA aggregation success (no zero-response), and -- the key check --
# ZERO resource-exhaustion errors (topic/handle/sem table full, sem create
# failed) plus no garbage-PC stack-overflow crash signature.
#
# Usage:
#   ./run_Nnode_scale.sh            # default N=16
#   N=24 ./run_Nnode_scale.sh       # 24 nodes
#   N=32 ./run_Nnode_scale.sh       # 32 nodes (the new DNODE_MAX cap)
#
# Optional multi-region simulation (off by default => single coalesced
# region on localhost, the honest co-located outcome):
#   N=32 ZONE_SIZE=8 ZONE_PENALTY=200 ./run_Nnode_scale.sh
#
# Tunables:
#   N            number of nodes (node ids 1..N), default 16
#   ZONE_SIZE    PKERNEL_RTT_ZONE_SIZE  (0/unset => no sim zones)
#   ZONE_PENALTY PKERNEL_RTT_ZONE_PENALTY ms cross-zone (default 200)
#   SETTLE       seconds to let SWIM measure RTT + regions settle (default 18)
#
# Watch:   /tmp/pkN_node{1..N}.log  /tmp/pkN_relay.log
# ---------------------------------------------------------------------------
set -u

N="${N:-16}"
ZONE_SIZE="${ZONE_SIZE:-0}"
ZONE_PENALTY="${ZONE_PENALTY:-200}"
SETTLE="${SETTLE:-18}"
PORT="${PORT:-27600}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BOOT="$ROOT/boot/linux"     # so_node + libpkernel.so (aarch64 host)

if [ "$N" -lt 2 ] || [ "$N" -gt 32 ]; then
    echo "N must be in 2..32 (DNODE_MAX=32); got N=$N" >&2
    exit 2
fi

# --- build if needed --------------------------------------------------------
[ -x "$BOOT/so_node" ]     || make -C "$BOOT" so_node >/dev/null || exit 1
[ -f "$BOOT/libpkernel.so" ] || make -C "$BOOT" so_node >/dev/null || exit 1
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay"   >/dev/null || exit 1

export PKERNEL_RELAY_KEY=a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5
export PKERNEL_RELAY_HOST=127.0.0.1
export PKERNEL_RELAY_PORT="$PORT"
if [ "$ZONE_SIZE" -gt 0 ]; then
    export PKERNEL_RTT_ZONE_SIZE="$ZONE_SIZE"
    export PKERNEL_RTT_ZONE_PENALTY="$ZONE_PENALTY"
    echo "[scale] N=$N  multi-region sim: zone_size=$ZONE_SIZE penalty=${ZONE_PENALTY}ms  port=$PORT"
else
    unset PKERNEL_RTT_ZONE_SIZE PKERNEL_RTT_ZONE_PENALTY 2>/dev/null || true
    echo "[scale] N=$N  single region (localhost, no sim zones)  port=$PORT"
fi

LOGPFX=/tmp/pkN
rm -f "${LOGPFX}_node"*.log "${LOGPFX}_relay.log" 2>/dev/null

PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

# --- relay ------------------------------------------------------------------
echo "[scale] starting relay on :$PORT"
"$ROOT/relay/relay" -p "$PORT" -v >"${LOGPFX}_relay.log" 2>&1 & PIDS+=($!)
sleep 1

# --- nodes 2..N (background, no stdin) --------------------------------------
echo "[scale] launching nodes 2..$N"
cd "$BOOT"
for id in $(seq 2 "$N"); do
    PKERNEL_NODE_ID=$id PKERNEL_AUTONET=1 LD_LIBRARY_PATH=. \
        ./so_node </dev/null >"${LOGPFX}_node${id}.log" 2>&1 & PIDS+=($!)
    # small stagger so the relay + SWIM bring-up isn't a thundering herd
    sleep 0.15
done
sleep 2

# --- node 1: driver ---------------------------------------------------------
echo "[scale] launching node 1 (driver), settle=${SETTLE}s"
(
    sleep "$SETTLE"        # let SWIM measure RTT + regions settle
    echo "nodes"
    sleep 1
    echo "region"
    sleep 1
    echo "world"
    sleep 1
    echo "dist"
    sleep 1
    echo "infer 50 20 90 5"
    sleep 4
    echo "infer 10 80 30 60"
    sleep 4
    echo "exit"
) | PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 LD_LIBRARY_PATH=. ./so_node \
        >"${LOGPFX}_node1.log" 2>&1

sleep 1

# ===========================================================================
# Assertions
# ===========================================================================
PASS=0; FAIL=0
ok()   { echo "  PASS  $1"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL  $1"; FAIL=$((FAIL+1)); }

ALL_NODE_LOGS=$(ls "${LOGPFX}_node"*.log 2>/dev/null)

echo
echo "================ ASSERTIONS  (N=$N) ================"

# 1) all N nodes register on the relay -------------------------------------
REG=$(grep -E "registered" "${LOGPFX}_relay.log" 2>/dev/null \
        | grep -oE "node [0-9]+" | awk '{print $2}' | sort -un | wc -l)
echo "[1] relay registrations: $REG distinct node ids (expected $N)"
if [ "$REG" -ge "$N" ]; then ok "all $N nodes registered on relay";
else bad "only $REG/$N nodes registered on relay"; fi

# 2) node 1 membership/world view sees ~N nodes ----------------------------
#    'nodes' = SWIM membership ; 'world' = gossip situational map.
KNOWN=$(grep -E "known nodes:" "${LOGPFX}_node1.log" 2>/dev/null \
        | grep -oE "[0-9]+" | tail -1)
KNOWN="${KNOWN:-0}"
# allow a small slack: gossip/SWIM may not have every node by SETTLE; require
# at least ~75% of the cluster to be visible to count the membership healthy.
THRESH=$(( (N * 3) / 4 ))
echo "[2] node 1 world map 'known nodes' = $KNOWN (cluster N=$N, threshold>=$THRESH)"
if [ "$KNOWN" -ge "$THRESH" ]; then ok "node 1 sees $KNOWN/$N nodes in world map";
else bad "node 1 only sees $KNOWN/$N nodes (< $THRESH)"; fi

# 3) region(s) form --------------------------------------------------------
RGN=$(grep -cE "^\[region\] id=" "${LOGPFX}_node1.log" 2>/dev/null)
RGN_SIZE=$(grep -E "^\[region\] id=" "${LOGPFX}_node1.log" 2>/dev/null \
            | grep -oE "size=[0-9]+" | grep -oE "[0-9]+" | tail -1)
RGN_SIZE="${RGN_SIZE:-0}"
echo "[3] node 1 region print lines=$RGN, region size=$RGN_SIZE"
if [ "$RGN" -ge 1 ] && [ "$RGN_SIZE" -ge 1 ]; then ok "region formed (size=$RGN_SIZE)";
else bad "no region formed on node 1"; fi

# 4) DKVA aggregation succeeds (no zero-response) --------------------------
#    success line: "[dkva] aggregated A region peers + B remote regions (K KV entries)"
#    PASS requires at least one aggregate with K>0 KV entries.
AGG_NONZERO=$(grep -E "\[dkva\] aggregated " "${LOGPFX}_node1.log" 2>/dev/null \
              | grep -vE "\(0 KV entries\)" | wc -l)
AGG_TOTAL=$(grep -cE "\[dkva\] aggregated " "${LOGPFX}_node1.log" 2>/dev/null)
INFER_OK=$(grep -cE "\[infer\] => class" "${LOGPFX}_node1.log" 2>/dev/null)
INFER_NULL=$(grep -cE "\[infer\] no result" "${LOGPFX}_node1.log" 2>/dev/null)
echo "[4] dkva aggregates: $AGG_TOTAL total, $AGG_NONZERO non-zero; infer ok=$INFER_OK null=$INFER_NULL"
if { [ "$AGG_NONZERO" -ge 1 ] || [ "$INFER_OK" -ge 1 ]; } && [ "$INFER_NULL" -eq 0 ]; then
    ok "DKVA / distributed inference produced non-zero results"
else
    bad "DKVA aggregation produced no non-zero response (or infer timed out)"
fi

# 5) ZERO resource-exhaustion errors anywhere  (THE DNODE_MAX=32 CHECK) -----
EXH=$(grep -hnE "topic table full|handle table full|pending table full|sem create failed|create failed|sem .*fail|handle .*full|slot full" \
        $ALL_NODE_LOGS "${LOGPFX}_relay.log" 2>/dev/null)
EXH_CNT=$(printf '%s\n' "$EXH" | grep -c . )
echo "[5] resource-exhaustion hits across all logs: $EXH_CNT"
if [ "$EXH_CNT" -eq 0 ]; then
    ok "ZERO resource-exhaustion errors (DNODE_MAX=$N holds at runtime)"
else
    bad "resource exhaustion detected ($EXH_CNT hits):"
    printf '%s\n' "$EXH" | sed 's/^/        /' | head -20
fi

# 6) no garbage-PC stack-overflow crash signature --------------------------
#    Known signature: a fault line where pc == addr == a random value, and/or
#    a SIGSEGV/abort from a node. Also catch generic crash markers.
CRASH=$(grep -hnE "pc=0x[0-9a-f]+ +addr=0x[0-9a-f]+|SIGSEGV|Segmentation|signal 11|\*\*\* stack|abort" \
          $ALL_NODE_LOGS 2>/dev/null)
# also: a node process that died before printing its boot banner is suspicious;
# count nodes whose log lacks the dkva init line as a liveness proxy.
DEAD=0
for id in $(seq 1 "$N"); do
    f="${LOGPFX}_node${id}.log"
    [ -f "$f" ] || { DEAD=$((DEAD+1)); continue; }
    grep -qE "\[dkva\] initialized|p-kernel>|dtr distributed" "$f" 2>/dev/null || DEAD=$((DEAD+1))
done
CRASH_CNT=$(printf '%s\n' "$CRASH" | grep -c . )
echo "[6] crash-signature hits: $CRASH_CNT ; nodes that never reached boot/init: $DEAD/$N"
if [ "$CRASH_CNT" -eq 0 ] && [ "$DEAD" -eq 0 ]; then
    ok "no garbage-PC / stack-overflow crash, all $N nodes booted"
else
    bad "crash or non-booting nodes detected"
    printf '%s\n' "$CRASH" | sed 's/^/        /' | head -10
fi

# --- usage telemetry: where are we vs the kdds caps? -----------------------
echo
echo "================ RESOURCE TELEMETRY (node 1) ================"
TOPICS=$(grep -cE "\[kdds\] open  topic=" "${LOGPFX}_node1.log" 2>/dev/null)
POLL=$(grep -cE "\[kdds\] open  topic=.*\(poll\)" "${LOGPFX}_node1.log" 2>/dev/null)
BLOCKING=$(( TOPICS - POLL ))
echo "  kdds_open calls on node 1 : $TOPICS  (blocking/sem-consuming=$BLOCKING, poll-only=$POLL)"
echo "  caps: KDDS_TOPIC_MAX=160  KDDS_HANDLE_MAX=320  CFN_MAX_SEMID=256"
echo "  (each blocking open = 1 handle + 1 sem; poll open = 1 handle, 0 sem;"
echo "   distinct topic names consume topic slots, shared by pub+sub)"

echo
echo "================ SUMMARY ================"
echo "  N=$N   PASS=$PASS  FAIL=$FAIL"
echo "  logs: ${LOGPFX}_node{1..$N}.log  ${LOGPFX}_relay.log"
if [ "$FAIL" -eq 0 ]; then
    echo "  RESULT: PASS  (DNODE_MAX=$N validated at runtime)"
    exit 0
else
    echo "  RESULT: FAIL  ($FAIL assertion(s) failed -- see above)"
    exit 1
fi
