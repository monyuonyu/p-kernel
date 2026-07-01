#!/bin/bash
# ---------------------------------------------------------------------------
# run_survival_l0.sh — host cert for the survival-loop L0 STATE bus
#   (docs/architecture/20-architecture/survival-loop.md §6-L0 / §9, GO-L0 CROWN-PRESERVING).
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
# HOST-PORTABLE: each arch runs only when THIS host can build AND run it. An
# arch whose build toolchain is genuinely absent, or whose foreign binary has
# no matching qemu-user, prints `SKIP (toolchain/runtime absent)` and is NOT
# counted as a failure. But an arch whose toolchain IS present and whose build
# or run then breaks STILL fails — a missing tool is a skip, real breakage is
# never silently skipped. At least one arch must build+run+certify on any host
# with a usable toolchain; if EVERY arch skips, the cert exits NON-zero
# (`[survival-l0] NO ARCH COULD RUN`) rather than report a hollow ALL PASS.
#
#   boot/linux        = aarch64-hosted: native `cc` on an aarch64 host, else
#                       aarch64-linux-gnu-gcc; runs via qemu-aarch64 off-host.
#   boot/linux_x86_64 = x86_64-hosted:  native `gcc` on an x86_64 host, else
#                       x86_64-linux-gnu-gcc; runs via qemu-x86_64 off-host.
#
# Exit 0 = at least one arch ran and every arch that ran has [state-axis]/
# [state-gossip] PASS with the monotone falsifier correctly RED.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FAIL=0
RAN=0

host_arch() {  # normalise uname -m (arm64 -> aarch64)
    case "$(uname -m)" in arm64) echo aarch64 ;; *) uname -m ;; esac
}

run_bin() {  # $1 = binary path -> drive `survival l0` and echo the cert block
    local bin="$1" host; host="$(host_arch)"
    # The cert is PURE in-process (no `net`, no relay): disable the galaxy so the
    # run binds NO listening socket (default hosted boot serves 7800+(id-1)).
    # Keeps this cert collision-free with concurrent fleet runs (e.g. 42_one_mind
    # on 7442 / 7800-7802) and is correct hygiene — a cert opens no ports.
    export PKERNEL_GALAXY=0
    # SYMMETRIC qemu wrapping: a foreign-arch binary runs under the matching
    # qemu-user; a native binary runs directly. one_arch() has already SKIPped
    # any arch whose qemu-user is absent, so the native fallbacks below are only
    # ever reached for a native binary.
    case "$bin" in
        */linux_x86_64/*)
            if [ "$host" != "x86_64" ] && command -v qemu-x86_64 >/dev/null 2>&1; then
                printf 'survival l0\nexit\n' | qemu-x86_64 "$bin" 2>/dev/null
            else
                printf 'survival l0\nexit\n' | "$bin" 2>/dev/null
            fi ;;
        */linux/*)
            if [ "$host" != "aarch64" ] && command -v qemu-aarch64 >/dev/null 2>&1; then
                printf 'survival l0\nexit\n' | qemu-aarch64 "$bin" 2>/dev/null
            else
                printf 'survival l0\nexit\n' | "$bin" 2>/dev/null
            fi ;;
        *)  printf 'survival l0\nexit\n' | "$bin" 2>/dev/null ;;
    esac
}

one_arch() {  # $1 = boot dir, $2 = human label, $3 = target arch (aarch64|x86_64)
    local boot="$ROOT/$1" label="$2" tgt="$3" host; host="$(host_arch)"
    [ -d "$boot" ] || { echo "[$label] SKIP (no $1)"; return; }

    # ---- HOST-PORTABILITY GATE: can THIS host build + run this arch? -------
    # Mirror the boot/<dir>/Makefile toolchain auto-detection: native compiler
    # on a same-arch host, the <tgt>-linux-gnu- cross compiler + qemu-<tgt>
    # otherwise. SKIP (NOT a failure) only when a needed tool is truly absent.
    local cc run=""
    if [ "$host" = "$tgt" ]; then
        case "$tgt" in aarch64) cc=cc ;; *) cc=gcc ;; esac   # mirror the Makefiles
    else
        cc="${tgt}-linux-gnu-gcc"
        run="qemu-${tgt}"
    fi
    if ! command -v "$cc" >/dev/null 2>&1; then
        echo "[$label] SKIP (toolchain/runtime absent)"; return
    fi
    if [ -n "$run" ] && ! command -v "$run" >/dev/null 2>&1; then
        echo "[$label] SKIP (toolchain/runtime absent)"; return
    fi
    # Committed to actually building+running this arch -> it counts.
    RAN=1

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

one_arch "boot/linux"        "aarch64-hosted" "aarch64"
one_arch "boot/linux_x86_64" "x86_64-hosted"  "x86_64"

if [ "$RAN" -eq 0 ]; then
    echo "[survival-l0] NO ARCH COULD RUN (no host toolchain/runtime for any arch)"; exit 2
fi
if [ "$FAIL" -eq 0 ]; then echo "[survival-l0] ALL PASS"; exit 0
else echo "[survival-l0] FAILURES ABOVE"; exit 1; fi
