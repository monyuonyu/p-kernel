#!/bin/bash
# ---------------------------------------------------------------------------
# run_swim_selfsuspect.sh — host cert for the SWIM SELF-SUSPICION de-storm
#   (arch/common/swim.c gossip_apply self-suspect branch; replica.c:387).
#
# THE STORM (root-caused from CI run 28340693132): when a node hears a gossip
# rumor that IT ITSELF is SUSPECT/DEAD it (1) scatters its FULL, UNPACED memory
# snapshot to all peers via replica_scatter_all() and (2) refutes (bumps its
# incarnation + re-asserts ALIVE). Under self-hosted CI CPU load a node routinely
# falls behind on PING/ACK and is FALSELY rumored SUSPECT; each such rumor fires a
# full unpaced scatter -> the scatter work makes it fall FURTHER behind -> more
# suspicion -> more scatter: a self-feeding STORM (artifacts show 7-8 DEATH-THROES
# per node, 1:1 with SELF-SUSPICION). The deaf node then FAILs one_mind/shared_mind.
#
# THE FIX (HOSTED-GATED, crown-neutral): throttle the scatter to at most once per
# SELF_SCATTER_MIN_MS (default 2000ms) while the REFUTE still fires EVERY time. The
# whole throttle lives under _TK_HOSTED_LIBC_; the bare-metal .text crown stays
# byte-identical (the dedicated crown-text-identity CI job enforces that).
#
# This drives the REAL gossip-ingest (gossip_apply) IN-PROCESS via `swimtest`:
#   [selfsuspect-refute]   LOAD-BEARING liveness: N (>=8) rapid self-SUSPECT rumors
#                  advance my_incarnation by exactly N (ALIVE re-asserted each time)
#                  — the throttle does NOT weaken refutation.
#   [selfsuspect-throttle] CURE: the replica_scatter_all() 断末魔 fired EXACTLY once
#                  for the whole burst (de-storm).
#   FALSIFIER [-DSWIM_NO_SCATTER_THROTTLE]: the throttle is reverted to the pre-fix
#                  unconditional scatter, so the burst fires N scatters and
#                  [selfsuspect-throttle] goes RED. The cert FAILS if the falsifier
#                  does NOT go red — proving the throttle actually has teeth.
#
# HOST-PORTABLE like run_survival_l0.sh: each arch runs only when THIS host can
# build AND run it; a genuinely-absent toolchain/runtime SKIPs (not a failure), a
# present-but-broken toolchain still FAILs. If EVERY arch skips, exit NON-zero
# ([swim-selfsuspect] NO ARCH COULD RUN) rather than report a hollow ALL PASS.
#
# Exit 0 = at least one arch built+ran with [selfsuspect-refute]/[selfsuspect-
# throttle] PASS in the CURE build AND [selfsuspect-throttle] correctly RED under
# the -DSWIM_NO_SCATTER_THROTTLE falsifier.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FAIL=0
RAN=0

CURE_CFLAGS="-DSWIM_SELFSUSPECT_CERT"
FALS_CFLAGS="-DSWIM_SELFSUSPECT_CERT -DSWIM_NO_SCATTER_THROTTLE"

host_arch() {  # normalise uname -m (arm64 -> aarch64)
    case "$(uname -m)" in arm64) echo aarch64 ;; *) uname -m ;; esac
}

run_bin() {  # $1 = binary path -> drive `swimtest` and echo the cert block
    local bin="$1" host; host="$(host_arch)"
    # Pure in-process cert: no galaxy socket (avoid port collisions with fleet runs).
    export PKERNEL_GALAXY=0
    case "$bin" in
        */linux_x86_64/*)
            if [ "$host" != "x86_64" ] && command -v qemu-x86_64 >/dev/null 2>&1; then
                printf 'swimtest\nexit\n' | qemu-x86_64 "$bin" 2>/dev/null
            else
                printf 'swimtest\nexit\n' | "$bin" 2>/dev/null
            fi ;;
        */linux/*)
            if [ "$host" != "aarch64" ] && command -v qemu-aarch64 >/dev/null 2>&1; then
                printf 'swimtest\nexit\n' | qemu-aarch64 "$bin" 2>/dev/null
            else
                printf 'swimtest\nexit\n' | "$bin" 2>/dev/null
            fi ;;
        *)  printf 'swimtest\nexit\n' | "$bin" 2>/dev/null ;;
    esac
}

one_arch() {  # $1 = boot dir, $2 = human label, $3 = target arch (aarch64|x86_64)
    local boot="$ROOT/$1" label="$2" tgt="$3" host; host="$(host_arch)"
    [ -d "$boot" ] || { echo "[$label] SKIP (no $1)"; return; }

    # ---- HOST-PORTABILITY GATE: can THIS host build + run this arch? -------
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
    RAN=1

    # ---- CURE: build with the cert flag -> exactly ONE scatter for the burst
    make -C "$boot" clean >/dev/null 2>&1
    if ! make -C "$boot" EXTRA_CFLAGS="$CURE_CFLAGS" >/dev/null 2>&1; then
        echo "[$label] BUILD FAILED (cure)"; FAIL=1; return
    fi
    local out; out="$(run_bin "$boot/p-kernel")"
    echo "$out" | grep -E '^\[(swim-selfsuspect|selfsuspect-refute|selfsuspect-throttle)\]'
    if echo "$out" | grep -q '^\[selfsuspect-refute\] PASS' \
       && echo "$out" | grep -q '^\[selfsuspect-throttle\].* PASS' \
       && echo "$out" | grep -q '^\[swim-selfsuspect\] PASS'; then
        echo "[$label] CURE PASS ([selfsuspect-refute] + [selfsuspect-throttle]=1 scatter)"
    else
        echo "[$label] CURE FAIL (expected refute PASS + throttle=1 scatter)"; FAIL=1
    fi

    # ---- FALSIFIER: -DSWIM_NO_SCATTER_THROTTLE -> throttle goes RED ---------
    make -C "$boot" clean >/dev/null 2>&1
    if ! make -C "$boot" EXTRA_CFLAGS="$FALS_CFLAGS" >/dev/null 2>&1; then
        echo "[$label] BUILD FAILED (falsifier)"; FAIL=1; return
    fi
    out="$(run_bin "$boot/p-kernel")"
    echo "$out" | grep -E '^\[(swim-selfsuspect|selfsuspect-throttle)\]'
    if echo "$out" | grep -q '^\[selfsuspect-throttle\].* FAIL' \
       && echo "$out" | grep -q '^\[swim-selfsuspect\] FAIL'; then
        echo "[$label] FALSIFIER correctly RED (unthrottled storm fires N scatters)"
    else
        echo "[$label] FALSIFIER DID NOT go RED — cert is toothless!"; FAIL=1
    fi
    make -C "$boot" clean >/dev/null 2>&1
}

one_arch "boot/linux"        "aarch64-hosted" "aarch64"
one_arch "boot/linux_x86_64" "x86_64-hosted"  "x86_64"

if [ "$RAN" -eq 0 ]; then
    echo "[swim-selfsuspect] NO ARCH COULD RUN (no host toolchain/runtime for any arch)"; exit 2
fi
if [ "$FAIL" -eq 0 ]; then echo "[swim-selfsuspect] ALL PASS"; exit 0
else echo "[swim-selfsuspect] FAILURES ABOVE"; exit 1; fi
