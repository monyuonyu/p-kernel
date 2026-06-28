#!/bin/bash
# ---------------------------------------------------------------------------
# run_survival_l0.sh — host cert for the survival-loop L0 STATE bus
#   (docs/architecture/survival-loop.md §6-L0 / §9, GO-L0 CROWN-PRESERVING).
#
# L0 is the back-bone slice: a per-node 2-bit STATE (ACTIVE / STRESSED, with
# HIBERNATING / DYING reserved) that rides in the SPARE bits 3-4 of the
# existing WORLD_BEACON.firing byte — NO wire change, the beacon stays 12 bytes
# — and is gossiped so peers read it via world_peer_state(). The whole loop is
# hosted-gated (_TK_HOSTED_LIBC_); the QEMU bare-metal crown is byte-identical.
#
# This drives the SHIPPED production symbols (world_self_state_step /
# world_self_state / world_peer_state, fed by the real intero_scalar S_n bus)
# IN-PROCESS via the `survival l0` shell verb:
#
#   [state-axis]   LOAD-BEARING: at ONE identical high scalar, force each S_n
#                  axis dominant in turn. THREAT@hi -> ACTIVE (the rally arm,
#                  never STRESSED); SURPRISE/FAULT/DEGRADE/LATENCY @hi ->
#                  STRESSED. Only the AXIS varies, so a divergent destination
#                  proves the response is axis-dependent (NOT monotone).
#   [state-gossip] stamp a committed state into the firing byte's spare bits,
#                  world_observe() it as a peer, read it back via
#                  world_peer_state(); a beacon with bits 3-4 = 0 reads ACTIVE
#                  (an old 12-byte node's automatic back-compat default).
#   FALSIFIER [state-monotone-NOT]: rebuilt with -DSURVIVAL_L0_MONOTONE the FSM
#                  ignores the axis ("high s -> STRESSED regardless"), so
#                  THREAT@hi goes STRESSED and the [state-axis] THREAT assertion
#                  FAILS (RED). Proves the cert has teeth — the axis-divergence
#                  is what is being certified, not the provisional band values.
#
# Both arms run on BOTH host arches (aarch64-hosted native + x86_64-hosted via
# qemu-x86_64 when present). Exit 0 = [state-axis]/[state-gossip] PASS and the
# monotone falsifier correctly goes RED.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FAIL=0

run_bin() {  # $1 = binary path -> drive `survival l0` and echo the cert block
    local bin="$1"
    # The cert is PURE in-process (no `net`, no relay): disable the galaxy so the
    # run binds NO listening socket (default hosted boot serves 7800+(id-1)).
    # Keeps this cert collision-free with concurrent fleet runs (e.g. 42_one_mind
    # on 7442 / 7800-7802) and is correct hygiene — a cert opens no ports.
    export PKERNEL_GALAXY=0
    case "$bin" in
        */linux_x86_64/*)
            if command -v qemu-x86_64 >/dev/null 2>&1 && [ "$(uname -m)" != "x86_64" ]; then
                printf 'survival l0\nexit\n' | qemu-x86_64 "$bin" 2>/dev/null
            else
                printf 'survival l0\nexit\n' | "$bin" 2>/dev/null
            fi ;;
        *)  printf 'survival l0\nexit\n' | "$bin" 2>/dev/null ;;
    esac
}

one_arch() {  # $1 = boot dir, $2 = human label
    local boot="$ROOT/$1" label="$2"
    [ -d "$boot" ] || { echo "[$label] SKIP (no $1)"; return; }

    # ---- CURE: default build -> [state-axis]/[state-gossip] PASS -----------
    make -C "$boot" clean >/dev/null 2>&1
    if ! make -C "$boot" >/dev/null 2>&1; then
        echo "[$label] BUILD FAILED (cure)"; FAIL=1; return
    fi
    local out; out="$(run_bin "$boot/p-kernel")"
    echo "$out" | grep -E '^\[(survival-l0|state-axis|state-gossip)\]'
    if echo "$out" | grep -q '^\[state-axis\] PASS' \
       && echo "$out" | grep -q '^\[state-gossip\] PASS' \
       && echo "$out" | grep -q '^\[survival-l0\] PASS'; then
        echo "[$label] CURE PASS ([state-axis] + [state-gossip])"
    else
        echo "[$label] CURE FAIL (expected [state-axis]/[state-gossip]/[survival-l0] PASS)"; FAIL=1
    fi

    # ---- FALSIFIER: -DSURVIVAL_L0_MONOTONE -> [state-axis] goes RED --------
    make -C "$boot" clean >/dev/null 2>&1
    if ! make -C "$boot" EXTRA_CFLAGS=-DSURVIVAL_L0_MONOTONE >/dev/null 2>&1; then
        echo "[$label] BUILD FAILED (falsifier)"; FAIL=1; return
    fi
    out="$(run_bin "$boot/p-kernel")"
    if echo "$out" | grep -q '^\[state-axis\] FAIL' \
       && echo "$out" | grep -q '^\[survival-l0\] FAIL'; then
        echo "[$label] FALSIFIER correctly RED ([state-monotone-NOT]: monotone FSM stresses THREAT)"
    else
        echo "[$label] FALSIFIER DID NOT go RED — cert is toothless!"; FAIL=1
    fi
    make -C "$boot" clean >/dev/null 2>&1
}

one_arch "boot/linux"        "aarch64-hosted"
one_arch "boot/linux_x86_64" "x86_64-hosted"

if [ "$FAIL" -eq 0 ]; then echo "[survival-l0] ALL PASS"; exit 0
else echo "[survival-l0] FAILURES ABOVE"; exit 1; fi
