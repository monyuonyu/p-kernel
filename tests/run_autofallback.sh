#!/bin/bash
# ---------------------------------------------------------------------------
# run_autofallback.sh — connect-anywhere SLICE 4 cert: AUTOMATIC relay-transport
# fallback (UDP<->TCP), the in-proc gate
# (arch/linux/*/net_dispatch.c::net_xport_select_self_test, driven by
# `autoxport test`).
#
# WHY: a node on a UDP-blocked net (corporate/cafe/some carriers) must STILL
# join the SAME relay mesh WITHOUT a human setting PKERNEL_RELAY_TCP. The
# selector brings up relay-UDP and, if the relay does not ANSWER within a bounded
# head start, races relay-TCP to the SAME endpoint and adopts whichever the relay
# answers first (UDP preferred on a tie), with a hysteresis so a short UDP blip
# never flaps the transport.
#
# This drives the SHIPPED selector IN-PROCESS (mock monotonic clock + a mock
# "network", NO sockets) via `autoxport test`:
#
#   CURE      : CASE A (UDP open) -> adopts relay-udp, NEVER inits relay-TCP;
#               CASE B (UDP blocked, TCP open) -> auto-adopts relay-tcp inside
#               the bounded window; HYSTERESIS (5 s UDP blip does NOT switch,
#               switch only after >=20 s continuous UDP). RESULT: <N>/<N> PASS.
#   FALSIFIER : rebuilt with -DAUTOXPORT_NOFALLBACK the selector becomes UDP-only
#               (never starts relay-TCP) so CASE B can't join -> RESULT: FAIL.
#               Proves the auto-fallback, not anything else, carries the join.
#
# Both arms run on BOTH host arches (aarch64-hosted native + x86_64-hosted via
# qemu-x86_64 when present). Exit 0 = cure PASSES and falsifier FAILS.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
FAIL=0

run_bin() {  # $1 = binary path -> drive `autoxport test` and echo the cert block
    local bin="$1"
    case "$bin" in
        */linux_x86_64/*)
            if command -v qemu-x86_64 >/dev/null 2>&1 && [ "$(uname -m)" != "x86_64" ]; then
                printf 'autoxport test\nexit\n' | qemu-x86_64 "$bin" 2>/dev/null
            else
                printf 'autoxport test\nexit\n' | "$bin" 2>/dev/null
            fi ;;
        *)  printf 'autoxport test\nexit\n' | "$bin" 2>/dev/null ;;
    esac
}

one_arch() {  # $1 = boot dir, $2 = human label
    local boot="$ROOT/$1" label="$2"
    [ -d "$boot" ] || { echo "[$label] SKIP (no $1)"; return; }

    # ---- CURE: -DAUTOXPORT_CERT -> RESULT: <N>/<N> PASS -------------------
    make -C "$boot" clean >/dev/null 2>&1
    if ! make -C "$boot" EXTRA_CFLAGS=-DAUTOXPORT_CERT >/dev/null 2>&1; then
        echo "[$label] BUILD FAILED (cure)"; FAIL=1; return
    fi
    local out; out="$(run_bin "$boot/p-kernel")"
    echo "$out" | grep -E '^\[autoxport\]'
    if echo "$out" | grep -qE '\[autoxport\] RESULT: [0-9]+/[0-9]+ PASS'; then
        echo "[$label] CURE PASS"
    else
        echo "[$label] CURE FAIL (expected RESULT: <N>/<N> PASS)"; FAIL=1
    fi

    # ---- FALSIFIER: +-DAUTOXPORT_NOFALLBACK -> RESULT: FAIL (teeth) -------
    make -C "$boot" clean >/dev/null 2>&1
    if ! make -C "$boot" EXTRA_CFLAGS="-DAUTOXPORT_CERT -DAUTOXPORT_NOFALLBACK" >/dev/null 2>&1; then
        echo "[$label] BUILD FAILED (falsifier)"; FAIL=1; return
    fi
    out="$(run_bin "$boot/p-kernel")"
    if echo "$out" | grep -q '\[autoxport\] RESULT: FAIL'; then
        echo "[$label] FALSIFIER correctly FAILS (cert has teeth)"
    else
        echo "[$label] FALSIFIER DID NOT FAIL — cert is toothless!"; FAIL=1
    fi
    make -C "$boot" clean >/dev/null 2>&1
}

one_arch "boot/linux"        "aarch64-hosted"
one_arch "boot/linux_x86_64" "x86_64-hosted"

if [ "$FAIL" -eq 0 ]; then echo "[autofallback] ALL PASS"; exit 0
else echo "[autofallback] FAILURES ABOVE"; exit 1; fi
