#!/bin/bash
# tests/host/run_n2_lan.sh — [n2-lan] a wide id on the LAN transport (ROADMAP N2).
#
# net_lan.c turned every id outside 1..255 into node 1, so a wide node 300 with
# PKERNEL_LAN=1 took node 1's place on the LAN. Now it refuses, like the TCP
# transport, and the node falls back (relay or loopback). Judged from stderr:
#   L1  PKERNEL_NODE_ID=300: "is a wide id: not on LAN", "lan-direct init failed",
#       and no "transport = lan-direct (node 1)"
#   L2  PKERNEL_NODE_ID=3: "transport = lan-direct (node 3)" (a narrow id unchanged)
#   L3  loopback (no LAN, no relay), PKERNEL_NODE_ID=300: "is a wide id: not on loopback",
#       "transport = none", and never node 1 (net_unix.c used to make it node 1)
#   L3b the range's edges on loopback: 257 and 65534 refused, 256 and 65535 not called wide
#   L4  loopback, PKERNEL_NODE_ID=3: "transport = loopback (node 3)"
#   L5  PKERNEL_RELAY_TCP=1 without the key: wide 300, 257, 65534 refused on TCP (v3 needs the MAC), 256 not called wide
# Negative controls: -DN2_NC_LAN_WIDE_TO_1 (the old LAN rule) turns L1 red,
# -DN2_NC_LOOP_WIDE_TO_1 (the old loopback rule) turns L3 red.
# Limit: ids 9..256 and a non-number still become node 1 on loopback (narrow ids
# beyond the 8 ports; not part of N2).
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
    ( printf 'exit\n'; sleep 2 ) | PKERNEL_AUTONET=1 PKERNEL_LAN=1 PKERNEL_LAN_PORT="$2" PKERNEL_NODE_ID="$1" \
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
# loopback (net_unix.c, no LAN and no relay): L3 a wide id is refused there too and the
# node runs without a network (never node 1); L4 a narrow id is unchanged
loop() {  # loop <id> <log>
    ( printf 'exit\n'; sleep 2 ) | env -u PKERNEL_RELAY -u PKERNEL_RELAY_HOST -u PKERNEL_SEED -u PKERNEL_LAN \
        PKERNEL_AUTONET=1 PKERNEL_NODE_ID="$1" timeout 20 "$BIN" > "$W/$2.out" 2> "$W/$2.err" || true
}
loop 300 lwide
loop 3 lnarrow
if grep -aqF 'PKERNEL_NODE_ID=300 is a wide id: not on loopback' "$W/lwide.err" &&
   grep -aqF 'transport = none' "$W/lwide.err" &&
   ! grep -aqF 'transport = loopback (node 1)' "$W/lwide.err" &&
   ! grep -aqF 'node 1 listening' "$W/lwide.err"; then
    echo "[n2-lan] L3 PASS: wide 300 is refused on loopback, no network, not node 1"
else
    echo "[n2-lan] L3 FAIL: wide 300 on loopback: $(grep -a 'net_unix\|transport' "$W/lwide.err" | head -3 | tr '\n' '|')"
    fail=1
fi
# L3b the edges of the wide range (257..65534, net_unix.c): 257 and 65534 are refused,
# 256 and 65535 are not called wide (test-holes H19: `> 257` or `< 65534` stayed green)
loop 257 lw257; loop 65534 lw65534; loop 256 ln256; loop 65535 ln65535
l3b=""
for i in 257 65534; do
    grep -aqF "PKERNEL_NODE_ID=$i is a wide id: not on loopback" "$W/lw$i.err" || l3b="$l3b $i-not-refused"
done
for i in 256 65535; do
    grep -aqF "PKERNEL_NODE_ID=$i is a wide id" "$W/ln$i.err" && l3b="$l3b $i-called-wide"
done
if [ -z "$l3b" ]; then
    echo "[n2-lan] L3b PASS: 257 and 65534 are refused on loopback, 256 and 65535 are not called wide"
else
    echo "[n2-lan] L3b FAIL: the wide range's edges on loopback:$l3b"
    fail=1
fi
if grep -aqF 'transport = loopback (node 3)' "$W/lnarrow.err"; then
    echo "[n2-lan] L4 PASS: narrow 3 is node 3 on loopback"
else
    echo "[n2-lan] L4 FAIL: narrow 3 on loopback: $(grep -a 'net_unix\|transport' "$W/lnarrow.err" | head -3 | tr '\n' '|')"
    fail=1
fi
# L5 the TCP relay transport without PKERNEL_RELAY_KEY (a v3 frame needs the MAC; with the key a wide
# id speaks v3 on TCP, run_n2_tcp.sh): refused before any connect ("is a wide id: not on TCP", "relay-tcp init failed") and never
# becomes relay-tcp node 1; the edges as L3b (257 and 65534 refused, 256 not called wide).
# No relay listens on the port: only the refusal is judged, not where the node goes after.
tcp() {  # tcp <id> <log>
    ( printf 'exit\n'; sleep 2 ) | env -u PKERNEL_RELAY_KEY -u PKERNEL_RELAY_HOST -u PKERNEL_SEED -u PKERNEL_LAN \
        PKERNEL_AUTONET=1 PKERNEL_RELAY_TCP=1 PKERNEL_RELAY=127.0.0.1:17359 PKERNEL_RELAY_AUTOFALLBACK=0 \
        PKERNEL_NODE_ID="$1" timeout 20 "$BIN" > "$W/$2.out" 2> "$W/$2.err" || true
}
tcp 300 tw300; tcp 257 tw257; tcp 65534 tw65534; tcp 256 tn256
l5=""
for i in 300 257 65534; do
    grep -aqF "PKERNEL_NODE_ID=$i is a wide id: not on TCP" "$W/tw$i.err" || l5="$l5 $i-not-refused"
    grep -aqF 'relay-tcp init failed' "$W/tw$i.err" || l5="$l5 $i-no-fallback-line"
    grep -aqF 'transport = relay-tcp (node 1)' "$W/tw$i.err" && l5="$l5 $i-became-node-1"
done
grep -aqF 'PKERNEL_NODE_ID=256 is a wide id' "$W/tn256.err" && l5="$l5 256-called-wide"
if [ -z "$l5" ]; then
    echo "[n2-lan] L5 PASS: wide 300, 257 and 65534 are refused on TCP, 256 is not called wide"
else
    echo "[n2-lan] L5 FAIL: the TCP relay transport:$l5"
    fail=1
fi
[ "$fail" = 0 ] && echo "[n2-lan] ALL PASS" || echo "[n2-lan] RED"
exit "$fail"
