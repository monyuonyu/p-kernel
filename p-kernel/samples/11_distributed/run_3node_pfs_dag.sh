#!/bin/bash
# ---------------------------------------------------------------------------
# 3-node p-fs P2 version-DAG demo over the public ./relay.
#
# Node 1 saves TWO versions of one named object:
#     pfs save greeting hello     -> manifest m1 {prev=0,  content=H(hello), seq=1}
#     pfs save greeting world     -> manifest m2 {prev=m1, content=H(world), seq=2}
# Manifests and content are ordinary content-addressed blocks, so P1's
# announce/want replicates all four blocks to nodes 2 and 3 for free. The
# ref (greeting -> head manifest) gossips on the REGION topic pfs/ref and
# merges last-writer-wins by seq.
#
# Proof of "saving never destroys the past", read on a DIFFERENT node:
#     node2  pfs log greeting       -> shows BOTH versions (seq=2 then seq=1)
#     node2  pfs cat greeting       -> "world"  (head)
#     node2  pfs cat greeting @1    -> "hello"  (old version still alive)
#
# Usage:   ./run_3node_pfs_dag.sh
# Watch:   /tmp/pfsdag_node{1,2,3}.log  /tmp/pfsdag_relay.log
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
export PKERNEL_RELAY_PORT=7412

PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

echo "[demo] starting relay on :$PKERNEL_RELAY_PORT"
"$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" -v >/tmp/pfsdag_relay.log 2>&1 & PIDS+=($!)
sleep 1

echo "[demo] starting nodes 2 and 3 (readers)"
{ sleep 26; echo "pfs log greeting"; sleep 2; echo "pfs cat greeting"; sleep 2; \
  echo "pfs cat greeting @1"; sleep 2; echo "exit"; } | \
  PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 "$BOOT/p-kernel" >/tmp/pfsdag_node2.log 2>&1 & PIDS+=($!)
{ sleep 26; echo "pfs log greeting"; sleep 2; echo "pfs cat greeting"; sleep 2; \
  echo "exit"; } | \
  PKERNEL_NODE_ID=3 PKERNEL_AUTONET=1 "$BOOT/p-kernel" >/tmp/pfsdag_node3.log 2>&1 & PIDS+=($!)
sleep 2

echo "[demo] starting node 1: save greeting twice (hello -> world)"
{ sleep 10; echo "pfs save greeting hello"; sleep 4; echo "pfs save greeting world"; \
  sleep 8; echo "pfs log greeting"; sleep 2; echo "exit"; } | \
  PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 "$BOOT/p-kernel" >/tmp/pfsdag_node1.log 2>&1
wait "${PIDS[@]:1}" 2>/dev/null

for n in 1 2 3; do
  echo; echo "===== node $n ====="
  grep -E "\[pfs\] (saved|ref|cat|log|no )|^  seq=" "/tmp/pfsdag_node$n.log"
done
echo
echo "[demo] done. full logs in /tmp/pfsdag_node{1,2,3}.log /tmp/pfsdag_relay.log"
