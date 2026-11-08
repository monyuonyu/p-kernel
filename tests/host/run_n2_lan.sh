#!/bin/bash
# tests/host/run_n2_lan.sh — [n2-lan] a wide id on the LAN transport (ROADMAP N2).
#
# net_lan.c turned every id outside 1..255 into node 1, so a wide node 300 with
# PKERNEL_LAN=1 took node 1's place on the LAN. Now it refuses, like the TCP
# transport, and the node falls back (relay or loopback). Judged from stderr:
#   L1  PKERNEL_NODE_ID=300: "is a wide id: not on LAN", "lan-direct init failed",
#       and no "transport = lan-direct (node 1)"
#   L2  PKERNEL_NODE_ID=3: "transport = lan-direct (node 3)" (a narrow id unchanged)
# Negative control: a build with -DN2_NC_LAN_WIDE_TO_1 (the old rule) turns L1 red.
#
#   N2_LAN_BIN=<p-kernel>  (default boot/linux_x86_64/p-kernel of this tree)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BIN="${N2_LAN_BIN:-$ROOT/boot/linux_x86_64/p-kernel}"
W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
fail=0
run() {  # run <id> <port> <log>
    ( printf 'exit\n'; sleep 2 ) | PKERNEL_LAN=1 PKERNEL_LAN_PORT="$2" PKERNEL_NODE_ID="$1" \
        timeout 20 "$BIN" > "$W/$3.out" 2> "$W/$3.err" || true
}
run 300 17351 wide
run 3 17352 narrow
if grep -aqF 'PKERNEL_NODE_ID=300 is a wide id: not on LAN' "$W/wide.err" &&
   grep -aqF 'lan-direct init failed' "$W/wide.err" &&
   ! grep -aqF 'transport = lan-direct (node 1)' "$W/wide.err"; then
    echo "[n2-lan] L1 PASS: wide 300 is refused on the LAN and falls back"
else
    echo "[n2-lan] L1 FAIL: wide 300 on the LAN: $(grep -a 'net_lan\|transport' "$W/wide.err" | head -3 | tr '\n' '|')"
    fail=1
fi
if grep -aqF 'transport = lan-direct (node 3)' "$W/narrow.err"; then
    echo "[n2-lan] L2 PASS: narrow 3 is node 3 on the LAN"
else
    echo "[n2-lan] L2 FAIL: narrow 3: $(grep -a 'net_lan\|transport' "$W/narrow.err" | head -3 | tr '\n' '|')"
    fail=1
fi
[ "$fail" = 0 ] && echo "[n2-lan] ALL PASS" || echo "[n2-lan] RED"
exit "$fail"
