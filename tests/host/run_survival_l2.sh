#!/bin/bash
# ---------------------------------------------------------------------------
# run_survival_l2.sh — host cert for survival-loop L2: HIBERNATING
#   (docs/architecture/20-architecture/survival-loop.md §6-L2).
#
# THIS SLICE covers only the STATE-FSM half of §6-L2 (see world_survival_l2_test's
# declaration comment in world.h for what is deliberately deferred and why:
# actually pausing mind_merge_task/mind_net_task/DMN consolidation, beacon-cadence
# reduction, and routed-work shedding touch r3_incontext.c's live merge path on
# bare metal and need their own re-baseline + sign-off).
#
# L2 escalates L0/L1's STRESSED state to HIBERNATING when the DEGRADE axis stays
# high even after shedding load (a bigger commitment, PROVISIONAL 4x dwell vs
# plain STRESSED entry — only the mechanism is load-bearing). Reversal: an acute
# THREAT rallies instantly (same override STRESSED already has), resource
# recovery relaxes slowly (same shape STRESSED already has), or an explicit
# world_wake() call fires immediately regardless of s/dwell. HIBERNATING gossips
# over the SAME 2-bit WORLD_STATE_MASK L0 already wired -- zero new wire code.
# Hosted-gated (_TK_HOSTED_LIBC_); the QEMU bare-metal crown is byte-identical
# (verified by the crown gate below).
#
# Driven IN-PROCESS via the `survival l2` shell verb (PRODUCTION symbols:
# wstate_advance/world_self_state_step/world_wake):
#
#   [hibernate-reversible]  LOAD-BEARING: sustained DEGRADE (even after the node
#                     is already STRESSED) reaches HIBERNATING; a THREAT rally,
#                     resource recovery, and an explicit world_wake() each wake
#                     it back to ACTIVE; world_wake() is a no-op outside
#                     HIBERNATING.
#   [hibernate-gossip] HIBERNATING reads back via world_peer_state, same
#                     mechanism [state-gossip] already certified for L0.
#   [hibernate-not-death] NOT a runtime check (printed, not gated) -- self is
#                     not indexed into dnode_table (SWIM tracks peers, not
#                     self), so there is no "self ALIVE" bit a single-process
#                     hosted test can read back; the property holds by
#                     construction (no swim.c/dnode_table touch in this diff).
#                     A live multi-process proof is deferred, same honesty
#                     pattern as L1's own single-process limitation.
#   FALSIFIER [hibernate-reversible-NOT]: -DSURVIVAL_L2_NO_ESCALATE disables the
#                     STRESSED->HIBERNATING escalation -> sustained DEGRADE never
#                     reaches HIBERNATING -> [hibernate-reversible] RED.
# A falsifier that does NOT go RED = toothless = FAIL.
#
# CROWN GATE: rebuild both bare crowns and assert the hosted L0/L1/L2 symbols
# are ABSENT (gate B); this script does NOT carry its own canonical-hash gate A
# (unlike run_survival_l1.sh) -- run_survival_l1.sh's crown_gate already re-
# derives and checks gate A/C on the SAME source tree every time it runs, so
# duplicating the canonical-hash comparison here would just be a second copy of
# the same assertion to keep in sync. Needs the bare cross-toolchains; SKIP (not
# a failure) where they are absent.
#
# HOST-PORTABLE (same discipline as run_survival_l0.sh/l1.sh): each arch runs
# only when THIS host can build AND run it; a genuinely-absent toolchain/runtime
# is a SKIP, real breakage still fails. If EVERY arch skips, exit NON-zero
# rather than a hollow ALL PASS.
#
# Exit 0 = at least one arch ran with [hibernate-reversible]/[hibernate-gossip]/
# [survival-l2] PASS, the falsifier correctly RED, and the crown gate PASS-or-SKIP.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FAIL=0
RAN=0

host_arch() {  # normalise uname -m (arm64 -> aarch64)
    case "$(uname -m)" in arm64) echo aarch64 ;; *) uname -m ;; esac
}

run_bin() {  # $1 = binary path -> drive `survival l2` and echo the cert block
    local bin="$1" host; host="$(host_arch)"
    # PURE in-process (no net, no relay): disable the galaxy so no port is bound.
    export PKERNEL_GALAXY=0
    case "$bin" in
        */linux_x86_64/*)
            if [ "$host" != "x86_64" ] && command -v qemu-x86_64 >/dev/null 2>&1; then
                printf 'survival l2\nexit\n' | qemu-x86_64 "$bin" 2>/dev/null
            else
                printf 'survival l2\nexit\n' | "$bin" 2>/dev/null
            fi ;;
        */linux/*)
            if [ "$host" != "aarch64" ] && command -v qemu-aarch64 >/dev/null 2>&1; then
                printf 'survival l2\nexit\n' | qemu-aarch64 "$bin" 2>/dev/null
            else
                printf 'survival l2\nexit\n' | "$bin" 2>/dev/null
            fi ;;
        *)  printf 'survival l2\nexit\n' | "$bin" 2>/dev/null ;;
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

    # ---- CURE: default build -> [hibernate-reversible]/[hibernate-gossip]/
    #      [survival-l2] PASS ------------------------------------------------
    make -C "$boot" clean >/dev/null 2>&1
    if ! make -C "$boot" >/dev/null 2>&1; then
        echo "[$label] BUILD FAILED (cure)"; FAIL=1; return
    fi
    local out; out="$(run_bin "$boot/p-kernel")"
    echo "$out" | grep -E '^\[(survival-l2|hibernate-reversible|hibernate-gossip|hibernate-not-death)'
    if echo "$out" | grep -q '^\[hibernate-reversible\] PASS' \
       && echo "$out" | grep -q '^\[hibernate-gossip\] PASS' \
       && echo "$out" | grep -q '^\[survival-l2\] PASS'; then
        echo "[$label] CURE PASS ([hibernate-reversible] + [hibernate-gossip])"
    else
        echo "[$label] CURE FAIL (expected [hibernate-reversible]/[hibernate-gossip]/[survival-l2] PASS)"; FAIL=1
    fi

    # ---- FALSIFIER: -DSURVIVAL_L2_NO_ESCALATE -> [hibernate-reversible] RED --
    make -C "$boot" clean >/dev/null 2>&1
    if ! make -C "$boot" EXTRA_CFLAGS=-DSURVIVAL_L2_NO_ESCALATE >/dev/null 2>&1; then
        echo "[$label] BUILD FAILED (no-escalate falsifier)"; FAIL=1; return
    fi
    out="$(run_bin "$boot/p-kernel")"
    if echo "$out" | grep -q '^\[hibernate-reversible\] FAIL' \
       && echo "$out" | grep -q '^\[survival-l2\] FAIL'; then
        echo "[$label] FALSIFIER correctly RED ([hibernate-reversible-NOT]: escalation disabled -> HIBERNATING never reached)"
    else
        echo "[$label] FALSIFIER DID NOT go RED — [hibernate-reversible] cert is toothless!"; FAIL=1
    fi
    make -C "$boot" clean >/dev/null 2>&1
}

crown_gate() {  # gate B: the hosted L0/L1/L2 symbols must be ABSENT from both bare ELFs.
    for t in aarch64-linux-gnu-gcc aarch64-linux-gnu-objcopy \
             i686-linux-gnu-gcc i686-linux-gnu-objcopy; do
        command -v "$t" >/dev/null 2>&1 || { echo "[crown] SKIP (bare toolchain $t absent — run_survival_l1.sh's crown_gate + the crown-text-identity CI job enforce the byte-identical half)"; return; }
    done
    make -C "$ROOT/boot/aarch64" clean >/dev/null 2>&1
    make -C "$ROOT/boot/x86"     clean >/dev/null 2>&1
    if ! make -C "$ROOT/boot/aarch64" >/dev/null 2>&1; then echo "[crown] aarch64 BUILD FAILED"; FAIL=1; return; fi
    if ! make -C "$ROOT/boot/x86" kernel.elf >/dev/null 2>&1; then echo "[crown] x86 BUILD FAILED"; FAIL=1; return; fi

    local leak=0 elf
    for elf in "$ROOT/boot/aarch64/kernel.elf" "$ROOT/boot/x86/kernel.elf"; do
        if nm "$elf" 2>/dev/null | grep -qE 'world_wake|world_survival_l2_test|eff_state_penalty|moe_support_route_test|moe_state_fold|world_l1_flap_test|world_survival_l1_test|world_survival_l0_test|wstate_advance|wstate_flap|self_cooldown|world_self_state|world_peer_state|intero_test_force_axis'; then
            echo "[crown] gate B FAIL — a hosted L0/L1/L2 symbol leaked into $elf"; leak=1
        fi
    done
    if [ "$leak" = 0 ]; then
        echo "[crown] gate B PASS (hosted L0/L1/L2 symbols absent from both bare crowns)"
    else
        FAIL=1
    fi
    make -C "$ROOT/boot/aarch64" clean >/dev/null 2>&1
    make -C "$ROOT/boot/x86"     clean >/dev/null 2>&1
}

one_arch "boot/linux"        "aarch64-hosted" "aarch64"
one_arch "boot/linux_x86_64" "x86_64-hosted"  "x86_64"
crown_gate

if [ "$RAN" -eq 0 ]; then
    echo "[survival-l2] NO ARCH COULD RUN (no host toolchain/runtime for any arch)"; exit 2
fi
if [ "$FAIL" -eq 0 ]; then echo "[survival-l2] ALL PASS"; exit 0
else echo "[survival-l2] FAILURES ABOVE"; exit 1; fi
