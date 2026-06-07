#!/bin/bash
# ---------------------------------------------------------------------------
# 3-node p-fs P1 replication demo over the public ./relay.
#
# Node 1 stores a block (`pfs put hello`); the put-hook ANNOUNCEs it on the
# REGION-scoped K-DDS topic pfs/ann; nodes 2 and 3 (lacking the id) publish
# WANT and any holder streams the bytes over the private pmesh port 7382 in
# sfs-style 512B chunks. Each receiver re-hashes the bytes, verifies the
# block-id, and stores — so `pfs ls` on every node ends up showing the SAME
# block-id, length, and origin. No central index, no master copy.
#
# Usage:   ./run_3node_pfs.sh
# Watch:   /tmp/pfs_node{1,2,3}.log  /tmp/pfs_relay.log
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

PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

echo "[demo] starting relay on :$PKERNEL_RELAY_PORT"
"$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" -v >/tmp/pfs_relay.log 2>&1 & PIDS+=($!)
sleep 1

echo "[demo] starting nodes 2 and 3 (replication peers)"
{ sleep 22; echo "pfs ls"; sleep 2; echo "exit"; } | \
  PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 "$BOOT/p-kernel" >/tmp/pfs_node2.log 2>&1 & PIDS+=($!)
{ sleep 22; echo "pfs ls"; sleep 2; echo "exit"; } | \
  PKERNEL_NODE_ID=3 PKERNEL_AUTONET=1 "$BOOT/p-kernel" >/tmp/pfs_node3.log 2>&1 & PIDS+=($!)
sleep 2

echo "[demo] starting node 1: pfs put hello -> region replication"
{ sleep 10; echo "pfs put hello"; sleep 8; echo "pfs ls"; sleep 2; echo "exit"; } | \
  PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 "$BOOT/p-kernel" >/tmp/pfs_node1.log 2>&1
wait "${PIDS[@]:1}" 2>/dev/null

for n in 1 2 3; do
  echo; echo "===== node $n ====="
  grep -E "\[pfs\]|^  [0-9a-f]{16}  len=" "/tmp/pfs_node$n.log"
done
echo
echo "[demo] done. full logs in /tmp/pfs_node{1,2,3}.log /tmp/pfs_relay.log"
