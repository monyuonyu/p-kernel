#!/bin/bash
# ---------------------------------------------------------------------------
# R3a — the trained brain propagates through the network's memory.
#
# Node 1 proves the model is genuinely trained (no cherry-picking: both the
# BEFORE and AFTER held-out numbers are printed by the same `dtr eval`):
#     dtr eval        -> random LCG weights, ~26.7% held-out (chance ~33%)
#     dtr train       -> 300 epochs full-batch SGD, ANALYTIC backprop,
#                        real cross-entropy (~0.2 s on the host)
#     dtr eval        -> ~95% train / ~100% held-out (60 samples)
#     dtr save        -> weights -> content-addressed, VERSIONED p-fs object
#                        "dtr/weights" (P2 manifest + ref, P1 replication)
#
# Node 2 NEVER trains. It receives the weight blob via p-fs P1 chunk
# replication + P2 ref gossip, then:
#     dtr eval        -> same untrained ~26.7% (same deterministic init)
#     dtr load        -> pulls the blob out of its local replica
#     dtr eval        -> THE SAME trained accuracy as node 1
#
# If qemu-x86_64 and the x86_64 build are available, node 2 runs the
# OTHER ABI — the float32 blob is bit-identical across architectures, so
# the brain that learned on aarch64 wakes up inside an x86_64 kernel.
#
# Usage:   ./run_2node_train_propagate.sh
# Watch:   /tmp/r3a_node1.log  /tmp/r3a_node2.log  /tmp/r3a_relay.log
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux";        XBOOT="$ROOT/boot/linux_x86_64"; XQEMU=qemu-x86_64 ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64"; XBOOT="$ROOT/boot/linux";        XQEMU=qemu-aarch64 ;;
    *) echo "unsupported host arch $(uname -m)"; exit 1 ;;
esac

[ -x "$BOOT/p-kernel" ]    || make -C "$BOOT"        >/dev/null || exit 1
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay"  >/dev/null || exit 1

# node 2 runs the sibling ABI when possible (cross-arch weight portability)
NODE2_CMD=("$BOOT/p-kernel"); NODE2_ARCH="$(uname -m)"
if command -v "$XQEMU" >/dev/null 2>&1 && [ -x "$XBOOT/p-kernel" ]; then
    NODE2_CMD=("$XQEMU" "$XBOOT/p-kernel")
    NODE2_ARCH="${XQEMU#qemu-} (cross-ABI via $XQEMU)"
fi

export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
export PKERNEL_RELAY_HOST=127.0.0.1
export PKERNEL_RELAY_PORT=7414

PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

echo "[demo] starting relay on :$PKERNEL_RELAY_PORT"
"$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" -v >/tmp/r3a_relay.log 2>&1 & PIDS+=($!)
sleep 1

echo "[demo] starting node 2 (reader, $NODE2_ARCH) — it never trains"
{ sleep 38; echo "dtr eval";  sleep 3; echo "dtr load"; sleep 2; \
  echo "dtr eval"; sleep 3; echo "exit"; } | \
  PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 "${NODE2_CMD[@]}" >/tmp/r3a_node2.log 2>&1 & PIDS+=($!)
sleep 2

echo "[demo] starting node 1 (trainer, $(uname -m)): eval -> train -> eval -> save"
{
  sleep 8                       # let SWIM mesh the two nodes
  echo "dtr eval"               # BEFORE: random weights (honest baseline)
  sleep 2
  echo "dtr train"              # 300 epochs, analytic backprop, real CE
  sleep 4
  echo "dtr save"               # trained weights -> p-fs "dtr/weights"
  sleep 28                      # let P1 chunks + P2 ref gossip reach node 2
  echo "exit"
} | PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 "$BOOT/p-kernel" >/tmp/r3a_node1.log 2>&1
wait "${PIDS[@]:1}" 2>/dev/null

echo
echo "===== node 1 (trainer) ====="
grep -E "\[dtr\] (eval|training|trained|weights)|\[pfs\] saved" /tmp/r3a_node1.log
echo
echo "===== node 2 (reader, $NODE2_ARCH — never trained) ====="
grep -E "\[dtr\] (eval|weights|no )|\[pfs\] (ref|got block)" /tmp/r3a_node2.log
echo
echo "[demo] done. full logs in /tmp/r3a_node{1,2}.log /tmp/r3a_relay.log"
