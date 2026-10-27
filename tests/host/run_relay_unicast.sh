#!/bin/bash
# run_relay_unicast.sh — [relay-unicast] (ROADMAP N1). Builds
# tests/host/relay_unicast_test.c (it #includes arch/linux/x86_64/net_relay.c)
# and runs it. Seconds; no relay process, no kernel.
#   ./run_relay_unicast.sh      exit 0 = ALL PASS
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
CC=${CC:-gcc}
"$CC" -std=gnu11 -O1 -Wall -Wno-unused-result -I"$ROOT/arch/common/include" -I"$ROOT/relay" \
    "$HERE/relay_unicast_test.c" "$ROOT/relay/sha256.c" -o "$OUT/t" || {
    echo "[relay-unicast] FAIL build"; exit 1; }
"$OUT/t"
