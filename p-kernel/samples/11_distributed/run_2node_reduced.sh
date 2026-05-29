#!/bin/bash
# ---------------------------------------------------------------------------
# 2-node distributed Transformer inference (REDUCED / tensor parallel)
# over the public ./relay.
#
# Node 1 (the requester) computes attention head0 locally, ships the raw
# input to node 2 over the relay, node 2 computes head1 and returns it, and
# node 1 concatenates the heads to classify. This is the "Collective" layer
# of the p-kernel worldview made literal: one Transformer, two kernels.
#
# Usage:   ./run_2node_reduced.sh
# Watch:   /tmp/pk_node1.log  /tmp/pk_node2.log  /tmp/pk_relay.log
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"          # .../p-kernel/p-kernel

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "unsupported host arch $(uname -m)"; exit 1 ;;
esac

# Build kernel + relay if needed.
[ -x "$BOOT/p-kernel" ]    || make -C "$BOOT"        >/dev/null || exit 1
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay"  >/dev/null || exit 1

export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
export PKERNEL_RELAY_HOST=127.0.0.1
export PKERNEL_RELAY_PORT=7400

PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

echo "[demo] starting relay on :$PKERNEL_RELAY_PORT"
"$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" -v >/tmp/pk_relay.log 2>&1 & PIDS+=($!)
sleep 1

echo "[demo] starting node 2 (tensor-parallel worker)"
PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 "$BOOT/p-kernel" </dev/null >/tmp/pk_node2.log 2>&1 & PIDS+=($!)
sleep 2

echo "[demo] starting node 1 (requester) and driving inferences"
{
  sleep 5                              # let SWIM discover the peer -> REDUCED
  echo "dist"
  sleep 1
  echo "infer 50 20 90 5"
  sleep 2
  echo "infer 5 5 120 100"
  sleep 2
  echo "dtr"
  sleep 1
  echo "exit"
} | PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 "$BOOT/p-kernel" >/tmp/pk_node1.log 2>&1

echo
echo "===== node 1 (requester) ====="
grep -E "level|infer\] sensors|TP\(REDUCED\)|=> class|inferences" /tmp/pk_node1.log
echo
echo "===== node 2 (worker, head1 over relay) ====="
grep -E "TP: head1" /tmp/pk_node2.log | head -2
echo
echo "[demo] done. full logs in /tmp/pk_node1.log /tmp/pk_node2.log /tmp/pk_relay.log"
