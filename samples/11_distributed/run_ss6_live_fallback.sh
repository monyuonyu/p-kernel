#!/bin/bash
# ---------------------------------------------------------------------------
# run_ss6_live_fallback.sh — the [ss6-live] DEGRADE arm: a peer that owns some
# of node 1's wide experts is KILLED before the forward, so the live transport
# TIMES OUT on those experts and student.c recomputes them LOCALLY. The result
# is STILL byte-identical to the single-node hash (fallback loses WIDTH, not
# correctness), with an HONEST degraded count (fallback>0) and NO stall.
#
# Usage:   ./run_ss6_live_fallback.sh
# Exit 0 = live hash == single hash AND fallback>0 (a peer's experts were
#          recomputed locally after the wire timed out) AND no stall.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "unsupported"; exit 1 ;;
esac
[ -x "$BOOT/p-kernel" ]    || make -C "$BOOT"        >/dev/null || exit 1
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay"  >/dev/null || exit 1

export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
export PKERNEL_RELAY_HOST=127.0.0.1
export PKERNEL_RELAY_PORT=7412
export PKERNEL_REMOTE_EXPERTS=1
unset PKERNEL_RTT_ZONE_SIZE PKERNEL_RTT_ZONE_PENALTY

PIDS=(); declare -A NPID
cleanup() { kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

"$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" -v >/tmp/ss6lf_relay.log 2>&1 & PIDS+=($!)
sleep 1
for id in 2 3 4; do
  PKERNEL_NODE_ID=$id PKERNEL_AUTONET=1 "$BOOT/p-kernel" </dev/null \
      >/tmp/ss6lf_node${id}.log 2>&1 & NPID[$id]=$!; PIDS+=($!)
done
sleep 3

# node 1: form FULL (still 4 alive), capture the single oracle, THEN we kill a
# peer and run the live forward -> its owned experts must fall back locally.
# Drive node 1 through a FIFO so the outer shell can interleave the kill with
# millisecond precision: kill node 4, then within ~0.3s feed 'ss6live live' so
# node 4 is STILL ALIVE in SWIM/placement (failure detection needs ~2-4s) but
# unresponsive on SS6L_PORT -> the transport's 600ms timeout fires -> student.c
# recomputes that owner's experts LOCALLY (honest degraded), byte-identical.
FIFO="$(mktemp -u)"; mkfifo "$FIFO"
PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 "$BOOT/p-kernel" <"$FIFO" >/tmp/ss6lf_node1.log 2>&1 &
N1=$!; PIDS+=($!)
exec 9>"$FIFO"
sleep 8
echo "ss6live single" >&9       # single-node oracle (all 4 alive)
sleep 5
echo "[ss6-live] killing responder node 4 (still ALIVE in placement; unresponsive)"
kill -9 "${NPID[4]}" 2>/dev/null
sleep 0.3                        # << SWIM failure-detect window -> node4 still owner
echo "ss6live live" >&9         # live forward: node4-owned experts time out -> local
sleep 9
echo "exit" >&9
exec 9>&-
wait "$N1" 2>/dev/null
rm -f "$FIFO"

echo
echo "===== node 1 — fallback arm ====="
grep -iE 'ss6-live' /tmp/ss6lf_node1.log

SINGLE="$(grep -oE 'single  logit-hash=[0-9a-f]+' /tmp/ss6lf_node1.log | grep -oE '[0-9a-f]+$' | head -1)"
LIVE="$(grep -oE 'live    logit-hash=[0-9a-f]+'  /tmp/ss6lf_node1.log | grep -oE '[0-9a-f]+$' | head -1)"
FB="$(grep -oE 'fallback=[0-9]+' /tmp/ss6lf_node1.log | grep -oE '[0-9]+$' | head -1)"
FIRED="$(grep -oE 'remote_fired=[0-9]+' /tmp/ss6lf_node1.log | grep -oE '[0-9]+$' | head -1)"
: "${SINGLE:=MISS}"; : "${LIVE:=MISS}"; : "${FB:=0}"; : "${FIRED:=0}"
echo
echo "[ss6-live] single=$SINGLE  live(after kill)=$LIVE  remote_fired=$FIRED  fallback=$FB"
if [ "$SINGLE" != "MISS" ] && [ "$SINGLE" = "$LIVE" ] && [ "$FB" -ge 1 ]; then
    echo "[ss6-live] PASS  killed-peer fallback: $FB experts recomputed LOCALLY after"
    echo "           the wire timed out -> STILL byte-identical (honest degraded, no stall)"
    exit 0
elif [ "$SINGLE" = "$LIVE" ] && [ "$FB" -eq 0 ]; then
    echo "[ss6-live] OPEN: identical but fallback=0 — the dead peer owned none of the"
    echo "           wide experts this run (placement/timing). Re-run; not a fudge."
    exit 1
else
    echo "[ss6-live] DIVERGENCE under fallback: single=$SINGLE live=$LIVE fb=$FB"
    exit 1
fi
