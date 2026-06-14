#!/bin/bash
# ---------------------------------------------------------------------------
# Mesh-discovery cert (NET-DISCOVERY-STAR, wave-discovery-mesh).
#
# Two NON-HUB nodes over the ./relay with NO hub (the minimum-id node 0,
# i.e. relay id 1) present. The relay-wire id k maps to drpc node index k-1
# (MAC 52:54:00:00:00:0k), so PKERNEL_NODE_ID=2,3 are drpc nodes 1 and 2 —
# both non-hub; drpc node 0 is deliberately absent.
#
# Today (disease, on wave-i18n-galaxy) each node only ever PINGs the lowest
# non-self id (drpc node 0, the absent hub), so the two non-hub peers never
# directly probe each other and never form a region: region size=1, members =
# self only. With the fix (round-robin probe over the whole id space +
# transitive-membership gossip) they discover each other WITHOUT a hub and
# co-region: size=2.
#
# Naming note: region_print() prints drpc indices. The driven node (relay id 2)
# is drpc node 1 -> "node1(self)"; its peer (relay id 3) is drpc node 2 ->
# "node2". So PASS = the driven node shows size=2 with member "node2".
#
# PASS gate: "[mesh-discovery] PASS" iff the driven node's region size==2 and
# its peer (drpc node 2) is a member.
#
# Usage:   ./run_mesh_discovery.sh
# Watch:   /tmp/pkmd_node{2,3}.log  /tmp/pkmd_relay.log
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
export PKERNEL_RELAY_PORT=7421
# No RTT zone simulation: localhost RTT is tiny so both peers are well within
# tau — the ONLY thing that should keep them apart today is failed discovery.

PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

echo "[demo] starting relay on :$PKERNEL_RELAY_PORT"
"$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" -v >/tmp/pkmd_relay.log 2>&1 & PIDS+=($!)
sleep 1

echo "[demo] starting non-hub node 3 (id3)"
PKERNEL_NODE_ID=3 PKERNEL_AUTONET=1 "$BOOT/p-kernel" </dev/null >/tmp/pkmd_node3.log 2>&1 & PIDS+=($!)
sleep 2

echo "[demo] starting non-hub node 2 (id2) and querying region after SWIM settles"
{
  sleep 12                 # let SWIM gossip + probe RTT settle (no hub helping)
  echo "nodes"
  sleep 1
  echo "region"
  sleep 2
  echo "exit"
} | PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 "$BOOT/p-kernel" >/tmp/pkmd_node2.log 2>&1

# Drive node 3 region print too (via a second short-lived id3 would collide;
# instead read node3's autonet region print that cmd_net emits, plus its nodes).

echo
echo "===== driven node (relay id 2 = drpc node 1) — cluster + region view ====="
grep -E "\[cluster\]|discovered|\[region\]" /tmp/pkmd_node2.log | sed 's/^/  /'
echo
echo "===== peer node (relay id 3 = drpc node 2) — discovery view (autonet) ====="
grep -E "discovered|recovered|\[region\]" /tmp/pkmd_node3.log | sed 's/^/  /'
echo

# --- gate ----------------------------------------------------------------
# The driven node (drpc 1) must show a region of size 2 whose members include
# its peer drpc node 2 ("node2" in region_print's drpc-index naming).
n2_region_size=$(grep -E "\[region\] id=" /tmp/pkmd_node2.log | tail -1 | sed -n 's/.*size=\([0-9]*\).*/\1/p')
n2_has_peer=$(grep -E "\[region\] members:" /tmp/pkmd_node2.log | tail -1 | grep -cE "node2\(rtt")

echo "driven node: region size=${n2_region_size:-?}  peer(drpc2)-is-member=${n2_has_peer:-0}"
if [ "${n2_region_size:-0}" = "2" ] && [ "${n2_has_peer:-0}" -ge 1 ]; then
    echo "[mesh-discovery] PASS"
    exit 0
else
    echo "[mesh-discovery] FAIL — two non-hub nodes did not co-region (size != 2)"
    exit 1
fi
