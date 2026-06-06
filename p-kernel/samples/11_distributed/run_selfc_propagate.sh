#!/bin/bash
# ---------------------------------------------------------------------------
# selfc propagation demo — code written on one node becomes a living task
# inside another node's kernel.
#
#   node1   selfc save genome.c     -> the demo C source becomes a p-fs
#                                      object; its blocks announce to the
#                                      region exactly like any other data
#   (gossip: pfs/ann + pfs/ref over the relay)
#   node2   selfc run genome.c      -> reads the C source from ITS OWN
#                                      p-fs replica, compiles it in-process
#                                      with libtcc, and starts the result
#                                      as a new T-Kernel task
#
# No file was ever exchanged, no compiler process was ever forked: the
# source traveled as content-addressed blocks and was turned into machine
# code inside the receiving kernel's own address space.
#
# Requires a libtcc-enabled boot/linux build on node2's side (this script
# runs both nodes from the same binary, so just: apt-get install libtcc-dev
# && make -C boot/linux). Without libtcc, node2 will print the honest stub
# message instead — the demo then shows replication but not compilation.
#
# Usage:   ./run_selfc_propagate.sh
# Watch:   /tmp/selfc_node{1,2}.log  /tmp/selfc_relay.log
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
export PKERNEL_RELAY_PORT=7413

PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

echo "[demo] starting relay on :$PKERNEL_RELAY_PORT"
"$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" -v >/tmp/selfc_relay.log 2>&1 & PIDS+=($!)
sleep 1

echo "[demo] starting node 2 (will compile node 1's code inside itself)"
{ sleep 24; echo "selfc run genome.c"; sleep 8; echo "selfc ls"; sleep 2; \
  echo "exit"; } | \
  PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 "$BOOT/p-kernel" >/tmp/selfc_node2.log 2>&1 & PIDS+=($!)
sleep 2

echo "[demo] starting node 1 (authors the C source, saves it to p-fs)"
{ sleep 10; echo "selfc save genome.c"; sleep 26; echo "exit"; } | \
  PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 "$BOOT/p-kernel" >/tmp/selfc_node1.log 2>&1
wait "${PIDS[@]:1}" 2>/dev/null

echo; echo "===== node 1 (author) ====="
grep -E "\[selfc\]|\[pfs\] saved" /tmp/selfc_node1.log
echo; echo "===== node 2 (the code runs HERE) ====="
grep -E "\[selfc" /tmp/selfc_node2.log
echo
if grep -q "compiled at runtime inside the kernel" /tmp/selfc_node2.log; then
    echo "[demo] PASS — code written on node 1 is alive inside node 2's kernel"
else
    echo "[demo] FAIL — see /tmp/selfc_node{1,2}.log /tmp/selfc_relay.log"
    exit 1
fi
