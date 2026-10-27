#!/bin/bash
# run_relay_unicast.sh — [relay-unicast] (ROADMAP N1). Builds
# tests/host/relay_unicast_test.c four times — the UDP link net_relay.c and
# the TCP fallback net_relay_tcp.c (-DRU_TCP), each from arch/linux/x86_64 and
# arch/linux/aarch64 (-DRU_A64; plain C sockets, runs on the x86_64 host) —
# and runs them. Seconds; no relay process, no kernel.
#   ./run_relay_unicast.sh      exit 0 = ALL PASS on all four
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
CC=${CC:-gcc}
CF="-std=gnu11 -O1 -Wall -Wno-unused-result -I$ROOT/arch/common/include -I$ROOT/relay"
rc=0
for v in "udp:" "tcp:-DRU_TCP" "udp-a64:-DRU_A64" "tcp-a64:-DRU_TCP -DRU_A64"; do
    name="${v%%:*}"; defs="${v#*:}"
    # shellcheck disable=SC2086
    if "$CC" $CF $defs "$HERE/relay_unicast_test.c" "$ROOT/relay/sha256.c" -o "$OUT/$name"; then
        "$OUT/$name" || { echo "[relay-unicast] RED ($name)"; rc=1; }
    else
        echo "[relay-unicast] FAIL build ($name)"; rc=1
    fi
done
[ "$rc" = 0 ] && echo "[relay-unicast] CERT ALL PASS (4 links)"
exit "$rc"
