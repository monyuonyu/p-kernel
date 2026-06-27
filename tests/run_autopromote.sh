#!/bin/bash
# ---------------------------------------------------------------------------
# run_autopromote.sh — N-2d measured-capability supernode auto-promotion cert:
# the in-proc gate (arch/linux/*/supernode_autopromote.c::sap_self_test, driven
# by `autopromote test`).
#
# WHY: today a node becomes supernode-CAPABLE only by an explicit opt-in
# (PKERNEL_SUPERNODE=1). We want good supernodes BORN from measured fitness: a
# node AUTO-PROMOTES super_capable[self] when it MEASURES it is actually a good
# supernode (relay-reachable / public-or-cone / stable / not metered / low
# stress / not isolated), and AUTO-DEMOTES when it isn't. The election +
# capability gossip + N-2c forwarding are UNCHANGED; only the auto-SETTING of
# the bit is new.
#
# This drives the SHIPPED pure core sap_step() + live wrapper sap_evaluate()
# IN-PROCESS (mock monotonic clock + mock SAP_SIGNALS, NO sockets) via
# `autopromote test`:
#
#   CURE      : (A) CONE/public + stable -> PROMOTE only after the 60 s window;
#               (B) SYMMETRIC -> NEVER promotes (hard block);
#               (C) hysteresis (<60 s good blip never promotes, <30 s bad blip
#               never demotes); (D) PKERNEL_SUPERNODE=1 forces capable despite
#               all-bad signals (force>measured). RESULT: <N>/<N> PASS.
#   FALSIFIER : rebuilt with -DSAP_NO_SYMBLOCK the symmetric != clause is dropped
#               so the SYMMETRIC node in CASE B promotes -> the "never promotes"
#               assert flips -> RESULT: FAIL. Proves the symmetric HARD BLOCK,
#               not anything else, keeps an unpunchable node out of the role.
#
# Both arms run on BOTH host arches (aarch64-hosted native + x86_64-hosted via
# qemu-x86_64 when present). Exit 0 = cure PASSES and falsifier FAILS.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
FAIL=0

run_bin() {  # $1 = binary path -> drive `autopromote test` and echo the cert block
    local bin="$1"
    case "$bin" in
        */linux_x86_64/*)
            if command -v qemu-x86_64 >/dev/null 2>&1 && [ "$(uname -m)" != "x86_64" ]; then
                printf 'autopromote test\nexit\n' | qemu-x86_64 "$bin" 2>/dev/null
            else
                printf 'autopromote test\nexit\n' | "$bin" 2>/dev/null
            fi ;;
        *)  printf 'autopromote test\nexit\n' | "$bin" 2>/dev/null ;;
    esac
}

one_arch() {  # $1 = boot dir, $2 = human label
    local boot="$ROOT/$1" label="$2"
    [ -d "$boot" ] || { echo "[$label] SKIP (no $1)"; return; }

    # ---- CURE: -DSAP_CERT -> RESULT: <N>/<N> PASS ------------------------
    make -C "$boot" clean >/dev/null 2>&1
    if ! make -C "$boot" EXTRA_CFLAGS=-DSAP_CERT >/dev/null 2>&1; then
        echo "[$label] BUILD FAILED (cure)"; FAIL=1; return
    fi
    local out; out="$(run_bin "$boot/p-kernel")"
    echo "$out" | grep -E '^\[autopromote\]'
    if echo "$out" | grep -qE '\[autopromote\] RESULT: [0-9]+/[0-9]+ PASS'; then
        echo "[$label] CURE PASS"
    else
        echo "[$label] CURE FAIL (expected RESULT: <N>/<N> PASS)"; FAIL=1
    fi

    # ---- FALSIFIER: +-DSAP_NO_SYMBLOCK -> RESULT: FAIL (teeth) -----------
    make -C "$boot" clean >/dev/null 2>&1
    if ! make -C "$boot" EXTRA_CFLAGS="-DSAP_CERT -DSAP_NO_SYMBLOCK" >/dev/null 2>&1; then
        echo "[$label] BUILD FAILED (falsifier)"; FAIL=1; return
    fi
    out="$(run_bin "$boot/p-kernel")"
    if echo "$out" | grep -q '\[autopromote\] RESULT: FAIL'; then
        echo "[$label] FALSIFIER correctly FAILS (cert has teeth)"
    else
        echo "[$label] FALSIFIER DID NOT FAIL — cert is toothless!"; FAIL=1
    fi
    make -C "$boot" clean >/dev/null 2>&1
}

one_arch "boot/linux"        "aarch64-hosted"
one_arch "boot/linux_x86_64" "x86_64-hosted"

if [ "$FAIL" -eq 0 ]; then echo "[autopromote] ALL PASS"; exit 0
else echo "[autopromote] FAILURES ABOVE"; exit 1; fi
