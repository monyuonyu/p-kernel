#!/bin/bash
# ---------------------------------------------------------------------------
# run_ss6_live.sh — the SS-6 [ss6-live] cert: a REAL multi-process cross-node
# student forward over ./relay (wave-ss6-live).
#
# SS-6 proved IN-PROCESS (tests/llm/run_ss6.sh) that a MoE forward whose WIDE
# experts (chosen-slot j >= K_min) are computed by a peer == the single-node
# forward BYTE-IDENTICAL, with a hard timeout -> LOCAL fallback. This script
# cashes the DEFERRED [live] row: 4 p-kernel PROCESSES join one ./relay mesh;
# node 1 runs a student forward (`ss6live`) whose wide, PEER-owned experts
# (SS-5 HRW placement) are computed on the owning node over the wire (UDP
# SS6L_PORT) and summed in the SAME canonical order. The LIVE logit hash must
# equal node 1's OWN single-node oracle hash (hook OFF) -> byte-identical
# cross-node forward. A killed/absent owner -> local fallback (honest degraded).
#
# Why 4 nodes: the remote-expert gate fires only at degrade==FULL (>=3 alive
# PEERS, i.e. 4 total nodes) AND region_size()>=2. On localhost (no RTT zones)
# all 4 are ONE region, so region>=2 holds. With E=4 experts spread by HRW over
# 4 owners, node 1's WIDE experts land on peers -> real wire calls.
#
# Usage:   ./run_ss6_live.sh
# Watch:   /tmp/ss6l_node{1..4}.log  /tmp/ss6l_relay.log
# Exit 0 = the live logit-hash == the single-node hash AND >=1 expert went over
#          the wire (wire_sent>0). Exit 1 = a divergence (reported honestly).
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
export PKERNEL_RELAY_HOST=127.0.0.1
export PKERNEL_RELAY_PORT=7411
export PKERNEL_REMOTE_EXPERTS=1          # opt-in: arm the live remote-expert gate
unset PKERNEL_RTT_ZONE_SIZE PKERNEL_RTT_ZONE_PENALTY   # one region on localhost

PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

echo "[ss6-live] starting relay on :$PKERNEL_RELAY_PORT"
"$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" -v >/tmp/ss6l_relay.log 2>&1 & PIDS+=($!)
sleep 1

echo "[ss6-live] starting responder nodes 2,3,4 (PKERNEL_REMOTE_EXPERTS=1)"
for id in 2 3 4; do
  PKERNEL_NODE_ID=$id PKERNEL_AUTONET=1 "$BOOT/p-kernel" </dev/null \
      >/tmp/ss6l_node${id}.log 2>&1 & PIDS+=($!)
done
sleep 3

echo "[ss6-live] starting node 1 (requester); waiting for FULL degrade, then ss6live"
{
  sleep 8                                # SWIM discovers 3 peers -> degrade FULL
  echo "dist"                            # show degrade level
  sleep 1
  echo "ss6live"                         # single oracle + LIVE cross-node forward
  sleep 8                                # the L-tier-free M forward + wire calls
  echo "exit"
  sleep 1
} | PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 "$BOOT/p-kernel" >/tmp/ss6l_node1.log 2>&1

echo
echo "===== relay (process mesh) ====="
echo "relay clients seen: $(grep -c -iE 'client|join|conn' /tmp/ss6l_relay.log 2>/dev/null)"
echo "p-kernel processes launched: 4 (node1 requester + node2,3,4 responders)"
echo
echo "===== node 1 (requester) — degrade + ss6-live cert ====="
grep -iE 'level=|degrade|ss6-live' /tmp/ss6l_node1.log
echo
echo "===== responders served remote experts ====="
for id in 2 3 4; do
  echo "node $id bound: $(grep -c 'bound port 7378' /tmp/ss6l_node${id}.log)  (responder ready)"
done
echo

# ---- the gate: live hash == single hash, AND at least one expert went remote.
SINGLE="$(grep -oE 'single  logit-hash=[0-9a-f]+' /tmp/ss6l_node1.log | grep -oE '[0-9a-f]+$' | head -1)"
LIVE="$(grep -oE 'live    logit-hash=[0-9a-f]+'  /tmp/ss6l_node1.log | grep -oE '[0-9a-f]+$' | head -1)"
WIRE="$(grep -oE 'wire_sent=[0-9]+' /tmp/ss6l_node1.log | grep -oE '[0-9]+$' | head -1)"
FIRED="$(grep -oE 'remote_fired=[0-9]+' /tmp/ss6l_node1.log | grep -oE '[0-9]+$' | head -1)"
FALLBACK="$(grep -oE 'fallback=[0-9]+' /tmp/ss6l_node1.log | grep -oE '[0-9]+$' | head -1)"
: "${SINGLE:=MISSING}"; : "${LIVE:=MISSING}"; : "${WIRE:=0}"; : "${FIRED:=0}"; : "${FALLBACK:=0}"

echo "[ss6-live] single-node hash : $SINGLE"
echo "[ss6-live] LIVE (cross-node) : $LIVE"
echo "[ss6-live] over the wire     : remote_fired=$FIRED fallback=$FALLBACK wire_sent=$WIRE"
echo

# (the killed-peer fallback arm is exercised by run_ss6_live_fallback.sh, which
#  kills a responder mid-run; student.c then recomputes the absent owner's
#  experts LOCALLY -> still byte-identical, honest degraded, no stall.)

if [ "$SINGLE" = "MISSING" ] || [ "$LIVE" = "MISSING" ]; then
    echo "[ss6-live] OPEN: cert did not produce both hashes (see /tmp/ss6l_node1.log)"
    exit 1
fi
if [ "$SINGLE" = "$LIVE" ] && [ "$WIRE" -ge 1 ]; then
    echo "[ss6-live] PASS  byte-identical LIVE cross-node forward over $WIRE wire calls"
    echo "           (==single-node hash; >=1 expert computed on a PEER over ./relay)"
    exit 0
elif [ "$SINGLE" = "$LIVE" ] && [ "$WIRE" -eq 0 ]; then
    echo "[ss6-live] OPEN: hashes match but NO expert went over the wire (wire_sent=0)."
    echo "           The forward stayed local — the gate did not fire a peer-owned"
    echo "           wide expert (check degrade==FULL + region>=2 + HRW placement)."
    echo "           Byte-identity is then only the single-node path, not a LIVE proof."
    exit 1
else
    echo "[ss6-live] DIVERGENCE: live hash != single hash."
    echo "           single=$SINGLE  live=$LIVE  (fired=$FIRED fallback=$FALLBACK wire=$WIRE)"
    echo "           This is a REAL wire/order/float divergence — do NOT fudge green."
    exit 1
fi
