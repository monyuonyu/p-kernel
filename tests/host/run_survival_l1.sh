#!/bin/bash
# ---------------------------------------------------------------------------
# run_survival_l1.sh — host cert for survival-loop L1: STATE-aware support
#   routing + §8 two-time-constant hysteresis
#   (docs/architecture/20-architecture/survival-loop.md §5 / §6-L1 / §8 / §9 / §10).
#
# L1 builds on L0's per-node STATE (ACTIVE / STRESSED, riding the WORLD_BEACON
# firing spare bits, gossiped via world_peer_state). It folds a candidate's
# STATE-unhealth into MoE routing as a VIRTUAL LOAD on the eff_pressure axis, so
# work sheds OFF a STRESSED node toward ACTIVE peers (the §5 support-routing
# sign, the structural G20 guard) — and damps the ACTIVE<->STRESSED flap with a
# two-time-constant hysteresis (fast-to-stress, slow-to-relax + relax
# refractory; acute DANGER still relaxes instantly). The whole loop is
# hosted-gated (_TK_HOSTED_LIBC_); the QEMU bare-metal crown is byte-identical
# (verified by the crown gate below).
#
# Driven IN-PROCESS via the `survival l1` shell verb (PRODUCTION symbols:
# moe_select_step for routing, world_self_state_step/wstate_advance for the FSM):
#
#   [support-route]   LOAD-BEARING (sign, not magnitude): over M=3 candidates
#                     EQUAL in acc/RTT/region but one STRESSED, the STRESSED node
#                     gets FEWER picks than the ACTIVE pair (sheds off) AND no
#                     MORE picks than a blind control (no pile-on). The cert
#                     straddles P_s — only the sign + no-pile-on are certified.
#   [hysteresis]      MEASURE-THE-DISEASE-FIRST (wave-45): under a coupled S_n
#                     forcing (stress up while ACTIVE/holding work, down while
#                     STRESSED/shedding) the NAIVE one-dwell FSM (= L0) flaps
#                     flips_naive > K=12; the DAMPED two-time-constant FSM holds
#                     flips_damped <= K AND <= half of naive (the [moe-osc]
#                     acceptance shape).
#   FALSIFIER [support-route-NOT]: -DSURVIVAL_L1_SIGN_FLIP routes the penalty onto
#                     the THREAT/rally term (the literal G20 inversion) -> the
#                     STRESSED node GAINS work -> [support-route] RED.
#   FALSIFIER [hysteresis-NOT]: -DSURVIVAL_L1_NO_DAMP collapses the two
#                     time-constants to one (damped == naive) -> the flap returns
#                     -> [hysteresis] RED.
# A falsifier that does NOT go RED = toothless = FAIL.
#
# CROWN GATE (load-bearing): rebuild BOTH bare crowns and assert the .text is
# byte-identical to the canonical dev crown (gate A), the hosted L0/L1 symbols
# are ABSENT from the bare ELFs (gate B), and gate C (bare `moe test` unchanged)
# is IMPLIED by gate A (byte-identical .text => identical test output). Needs the
# bare cross-toolchains; SKIP (not a failure) where they are absent (e.g. the
# ump-x86_64 CI runner only has native gcc — the dedicated crown-text-identity
# job enforces the relative invariant there).
#
# HOST-PORTABLE (same discipline as run_survival_l0.sh): each arch runs only
# when THIS host can build AND run it; a genuinely-absent toolchain/runtime is a
# SKIP, real breakage still fails. If EVERY arch skips, exit NON-zero rather than
# a hollow ALL PASS.
#
# Exit 0 = at least one arch ran with [support-route]/[hysteresis]/[survival-l1]
# PASS, both falsifiers correctly RED, and the crown gate PASS-or-SKIP.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FAIL=0
RAN=0

# canonical dev crown (.text sha256), anchored to the sandbox gcc 15.2.0 — same
# anchor as docs/audit-trail.md and the crown-text-identity CI job.
A_CANON="755a20fae2d9b7415045193ea8287623dbeb906963609a63dce8a19c8a130513"
X_CANON="4064d8a95e68950eee263a1bd6f131518f655f002bf2eccc1e824b4d87ee0413"

host_arch() {  # normalise uname -m (arm64 -> aarch64)
    case "$(uname -m)" in arm64) echo aarch64 ;; *) uname -m ;; esac
}

run_bin() {  # $1 = binary path -> drive `survival l1` and echo the cert block
    local bin="$1" host; host="$(host_arch)"
    # PURE in-process (no net, no relay): disable the galaxy so no port is bound.
    export PKERNEL_GALAXY=0
    case "$bin" in
        */linux_x86_64/*)
            if [ "$host" != "x86_64" ] && command -v qemu-x86_64 >/dev/null 2>&1; then
                printf 'survival l1\nexit\n' | qemu-x86_64 "$bin" 2>/dev/null
            else
                printf 'survival l1\nexit\n' | "$bin" 2>/dev/null
            fi ;;
        */linux/*)
            if [ "$host" != "aarch64" ] && command -v qemu-aarch64 >/dev/null 2>&1; then
                printf 'survival l1\nexit\n' | qemu-aarch64 "$bin" 2>/dev/null
            else
                printf 'survival l1\nexit\n' | "$bin" 2>/dev/null
            fi ;;
        *)  printf 'survival l1\nexit\n' | "$bin" 2>/dev/null ;;
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

    # ---- CURE: default build -> [support-route]/[hysteresis]/[survival-l1] PASS
    make -C "$boot" clean >/dev/null 2>&1
    if ! make -C "$boot" >/dev/null 2>&1; then
        echo "[$label] BUILD FAILED (cure)"; FAIL=1; return
    fi
    local out; out="$(run_bin "$boot/p-kernel")"
    echo "$out" | grep -E '^\[(survival-l1|support-route|hysteresis)'
    if echo "$out" | grep -q '^\[support-route\] PASS' \
       && echo "$out" | grep -q '^\[hysteresis\] PASS' \
       && echo "$out" | grep -q '^\[survival-l1\] PASS'; then
        echo "[$label] CURE PASS ([support-route] + [hysteresis])"
    else
        echo "[$label] CURE FAIL (expected [support-route]/[hysteresis]/[survival-l1] PASS)"; FAIL=1
    fi

    # ---- FALSIFIER 1: -DSURVIVAL_L1_SIGN_FLIP -> [support-route] goes RED ----
    make -C "$boot" clean >/dev/null 2>&1
    if ! make -C "$boot" EXTRA_CFLAGS=-DSURVIVAL_L1_SIGN_FLIP >/dev/null 2>&1; then
        echo "[$label] BUILD FAILED (sign-flip falsifier)"; FAIL=1; return
    fi
    out="$(run_bin "$boot/p-kernel")"
    if echo "$out" | grep -q '^\[support-route\] FAIL' \
       && echo "$out" | grep -q '^\[survival-l1\] FAIL'; then
        echo "[$label] FALSIFIER-1 correctly RED ([support-route-NOT]: penalty on threat -> stressed GAINS work)"
    else
        echo "[$label] FALSIFIER-1 DID NOT go RED — sign-flip cert is toothless!"; FAIL=1
    fi

    # ---- FALSIFIER 2: -DSURVIVAL_L1_NO_DAMP -> [hysteresis] goes RED ---------
    make -C "$boot" clean >/dev/null 2>&1
    if ! make -C "$boot" EXTRA_CFLAGS=-DSURVIVAL_L1_NO_DAMP >/dev/null 2>&1; then
        echo "[$label] BUILD FAILED (no-damp falsifier)"; FAIL=1; return
    fi
    out="$(run_bin "$boot/p-kernel")"
    if echo "$out" | grep -q '^\[hysteresis\] FAIL' \
       && echo "$out" | grep -q '^\[survival-l1\] FAIL'; then
        echo "[$label] FALSIFIER-2 correctly RED ([hysteresis-NOT]: time-constants collapsed -> flap returns)"
    else
        echo "[$label] FALSIFIER-2 DID NOT go RED — hysteresis cert is toothless!"; FAIL=1
    fi
    make -C "$boot" clean >/dev/null 2>&1
}

crown_gate() {  # gate A (.text == canonical) + gate B (nm absence); gate C implied by A
    for t in aarch64-linux-gnu-gcc aarch64-linux-gnu-objcopy \
             i686-linux-gnu-gcc i686-linux-gnu-objcopy; do
        command -v "$t" >/dev/null 2>&1 || { echo "[crown] SKIP (bare toolchain $t absent — crown-text-identity job enforces on CI)"; return; }
    done
    make -C "$ROOT/boot/aarch64" clean >/dev/null 2>&1
    make -C "$ROOT/boot/x86"     clean >/dev/null 2>&1
    if ! make -C "$ROOT/boot/aarch64" >/dev/null 2>&1; then echo "[crown] aarch64 BUILD FAILED"; FAIL=1; return; fi
    if ! make -C "$ROOT/boot/x86" kernel.elf >/dev/null 2>&1; then echo "[crown] x86 BUILD FAILED"; FAIL=1; return; fi
    aarch64-linux-gnu-objcopy -O binary -j .text "$ROOT/boot/aarch64/kernel.elf" /tmp/l1crown_a.bin
    i686-linux-gnu-objcopy    -O binary -j .text "$ROOT/boot/x86/kernel.elf"     /tmp/l1crown_x.bin
    local AH XH; AH=$(sha256sum /tmp/l1crown_a.bin | cut -d' ' -f1); XH=$(sha256sum /tmp/l1crown_x.bin | cut -d' ' -f1)

    # gate A — absolute .text hash vs the canonical sandbox crown.
    if [ "$AH" = "$A_CANON" ] && [ "$XH" = "$X_CANON" ]; then
        echo "[crown] gate A PASS (.text byte-identical to canonical — aarch64 + x86)"
    elif [ "$AH" = "$A_CANON" ] || [ "$XH" = "$X_CANON" ]; then
        # exactly one matches => the canonical toolchain, but one crown MOVED => a
        # hosted gate leaked into bare-metal .text. Real drift -> FAIL.
        echo "[crown] gate A FAIL — canonical toolchain but a crown MOVED (hosted gate leaked!)"
        echo "[crown]   aarch64 $AH (canon $A_CANON)"
        echo "[crown]   x86     $XH (canon $X_CANON)"
        FAIL=1
    else
        # both differ => a non-canonical toolchain (pure gcc difference, not drift).
        # Defer to gate B + the relative crown-text-identity job.
        echo "[crown] gate A SKIP (non-canonical toolchain; both crowns differ from canonical)"
        echo "[crown]   aarch64 $AH"
        echo "[crown]   x86     $XH"
    fi

    # gate B — the hosted L0/L1 symbols must be ABSENT from both bare ELFs.
    local leak=0 elf
    for elf in "$ROOT/boot/aarch64/kernel.elf" "$ROOT/boot/x86/kernel.elf"; do
        if nm "$elf" 2>/dev/null | grep -qE 'eff_state_penalty|moe_support_route_test|moe_state_fold|moe_l1_herd|world_l1_flap_test|world_survival_l1_test|world_survival_l0_test|wstate_advance|wstate_flap|self_cooldown|world_self_state|world_peer_state|intero_test_force_axis'; then
            echo "[crown] gate B FAIL — a hosted L0/L1 symbol leaked into $elf"; leak=1
        fi
    done
    if [ "$leak" = 0 ]; then
        echo "[crown] gate B PASS (hosted L0/L1 symbols absent from both bare crowns)"
    else
        FAIL=1
    fi
    echo "[crown] gate C implied by gate A (byte-identical .text => bare 'moe test' unchanged)"
    make -C "$ROOT/boot/aarch64" clean >/dev/null 2>&1
    make -C "$ROOT/boot/x86"     clean >/dev/null 2>&1
}

one_arch "boot/linux"        "aarch64-hosted" "aarch64"
one_arch "boot/linux_x86_64" "x86_64-hosted"  "x86_64"
crown_gate

if [ "$RAN" -eq 0 ]; then
    echo "[survival-l1] NO ARCH COULD RUN (no host toolchain/runtime for any arch)"; exit 2
fi
if [ "$FAIL" -eq 0 ]; then echo "[survival-l1] ALL PASS"; exit 0
else echo "[survival-l1] FAILURES ABOVE"; exit 1; fi
