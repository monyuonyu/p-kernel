#!/bin/bash
# ---------------------------------------------------------------------------
# 3-node distributed KV attention (FULL mode) over the public ./relay.
#
# With 3 live nodes the degrade controller selects FULL, so dtr_infer takes
# the DKVA path: node 1 broadcasts its query Q across the mesh, every node
# computes partial attention over its local KV cache and replies, and node 1
# aggregates the partials into the final attention.
#
# Usage:   ./run_3node_full.sh
# Watch:   /tmp/pk3_node{1,2,3}.log  /tmp/pk3_relay.log
#
# Two earlier limitations are now fixed (see ../../arch/common/dkva.c):
#   * Per-source response topics ("dtr/dkva/resp/<node>") give each responder
#     its own LATEST_ONLY slot, so the requester now aggregates ALL peers per
#     round ("aggregated 2 peers") instead of just one.
#   * Each node seeds its KV cache with node-specific synthetic inputs at
#     startup (dtr_seed_kv_cache), so partials are non-trivial (entries=3) and
#     the aggregated attention genuinely combines the cluster's collective
#     memory rather than reducing to all-zeros.
#   This script runs full FULL/DKVA distributed attention across three kernels
#   over the relay: Q broadcast + per-source partial replies + aggregation.
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
export PKERNEL_RELAY_PORT=7400

PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

echo "[demo] starting relay on :$PKERNEL_RELAY_PORT"
"$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" -v >/tmp/pk3_relay.log 2>&1 & PIDS+=($!)
sleep 1

echo "[demo] starting nodes 2 and 3 (DKVA responders)"
PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 "$BOOT/p-kernel" </dev/null >/tmp/pk3_node2.log 2>&1 & PIDS+=($!)
PKERNEL_NODE_ID=3 PKERNEL_AUTONET=1 "$BOOT/p-kernel" </dev/null >/tmp/pk3_node3.log 2>&1 & PIDS+=($!)
sleep 2

echo "[demo] starting node 1 (requester) and driving inferences"
{
  sleep 6                              # let SWIM discover both peers -> FULL
  echo "dist"
  sleep 1
  echo "infer 50 20 90 5"
  sleep 2
  echo "infer 5 5 120 100"
  sleep 2
  echo "exit"
} | PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 "$BOOT/p-kernel" >/tmp/pk3_node1.log 2>&1

echo
echo "===== node 1 (requester) ====="
grep -E "level|Q broadcast|resp from node|aggregated|DKVA.*cluster|=> class" /tmp/pk3_node1.log
echo
echo "===== responders ====="
echo "node 2 replied: $(grep -c 'responded to node' /tmp/pk3_node2.log) time(s)"
echo "node 3 replied: $(grep -c 'responded to node' /tmp/pk3_node3.log) time(s)"
echo
echo "[demo] done. full logs in /tmp/pk3_node{1,2,3}.log /tmp/pk3_relay.log"
