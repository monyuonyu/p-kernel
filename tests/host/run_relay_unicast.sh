#!/bin/bash
# run_relay_unicast.sh — [relay-unicast] (ROADMAP N1). Builds
# tests/host/relay_unicast_test.c twice — over arch/linux/x86_64/net_relay.c
# (UDP) and net_relay_tcp.c (-DRU_TCP, the TCP fallback link) — and runs both.
# Seconds; no relay process, no kernel.
#   ./run_relay_unicast.sh      exit 0 = ALL PASS on both links
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
CC=${CC:-gcc}
CF="-std=gnu11 -O1 -Wall -Wno-unused-result -I$ROOT/arch/common/include -I$ROOT/relay"
rc=0
# shellcheck disable=SC2086
"$CC" $CF "$HERE/relay_unicast_test.c" "$ROOT/relay/sha256.c" -o "$OUT/udp" &&
    "$OUT/udp" || { echo "[relay-unicast] RED (udp)"; rc=1; }
# shellcheck disable=SC2086
"$CC" $CF -DRU_TCP "$HERE/relay_unicast_test.c" "$ROOT/relay/sha256.c" -o "$OUT/tcp" &&
    "$OUT/tcp" || { echo "[relay-unicast-tcp] RED (tcp)"; rc=1; }
exit "$rc"
