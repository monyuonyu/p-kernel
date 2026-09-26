#!/bin/bash
# ---------------------------------------------------------------------------
# run_mind_pause.sh — host cert for survival-loop L2's deferred pause-half
#   (docs/architecture/20-architecture/survival-loop.md §6-L2): while
#   HIBERNATING, mind_net_task and mind_merge_task (arch/common/r3_incontext.c)
#   skip their per-tick poll/announce/fold work.
#
# THIS SLICE covers ONLY the pause GATE (mind_paused_for_hibernation(), a
# single hosted-only predicate both tasks check at the top of their loop) --
# NOT beacon-cadence reduction or routed-work shedding, still deferred.
#
# [mind-pause] (r3_mind_pause_test, `mind pause` shell verb): drives the SAME
# world-state FSM L2's own [hibernate-reversible] cert drives
# (intero_test_force_axis + world_self_state_step, world_wake()) and checks
# mind_paused_for_hibernation() -- the EXACT predicate the production tasks
# gate on -- tracks ACTIVE -> HIBERNATING -> ACTIVE correctly. Does NOT drive
# the actual task loops through real ticks (multi-second real waits, and
# mind_merge_task needs a >=2-node region to do anything at all) -- same
# honesty pattern as [hibernate-not-death]'s printed-not-gated limitation.
#
# FALSIFIER [mind-pause-NOT]: -DMIND_PAUSE_NO_GATE makes the predicate always
# return "not paused" -> the HIBERNATING check in [mind-pause] goes RED. A
# falsifier that does NOT go RED = toothless = FAIL.
#
# CROWN: r3_incontext.c links into BOTH bare-metal crowns (unlike fed_id.h),
# so unlike run_federation_f1.sh this is NOT zero-impact by file-inclusion
# alone -- the guard is `#ifdef _TK_HOSTED_LIBC_`, which bare-metal builds
# never define, so the new code should compile to nothing there. This gate
# PROVES that: rebuild both bare crowns at HEAD and compare full-file sha256
# against HEAD~N (the commit before this slice, passed as $MIND_PAUSE_BASE_REF,
# default computed from git). Byte-identical = the #ifdef held.
#
# HOST-PORTABLE (same discipline as run_survival_l2.sh): each arch runs only
# when THIS host can build AND run it; a genuinely-absent toolchain/runtime
# is a SKIP, real breakage still fails. If EVERY arch skips, exit NON-zero
# rather than a hollow ALL PASS.
#
# Exit 0 = at least one arch ran with [mind-pause] PASS, the falsifier
# correctly RED, and the crown gate PASS-or-SKIP.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FAIL=0
RAN=0

host_arch() {  # normalise uname -m (arm64 -> aarch64)
    case "$(uname -m)" in arm64) echo aarch64 ;; *) uname -m ;; esac
}

run_bin() {  # $1 = binary path -> drive `mind pause` and echo the cert block
    local bin="$1" host; host="$(host_arch)"
    export PKERNEL_GALAXY=0
    case "$bin" in
        */linux_x86_64/*)
            if [ "$host" != "x86_64" ] && command -v qemu-x86_64 >/dev/null 2>&1; then
                printf 'mind pause\nexit\n' | qemu-x86_64 "$bin" 2>/dev/null
            else
                printf 'mind pause\nexit\n' | "$bin" 2>/dev/null
            fi ;;
        */linux/*)
            if [ "$host" != "aarch64" ] && command -v qemu-aarch64 >/dev/null 2>&1; then
                printf 'mind pause\nexit\n' | qemu-aarch64 "$bin" 2>/dev/null
            else
                printf 'mind pause\nexit\n' | "$bin" 2>/dev/null
            fi ;;
        *)  printf 'mind pause\nexit\n' | "$bin" 2>/dev/null ;;
    esac
}

one_arch() {  # $1 = boot dir, $2 = human label, $3 = target arch (aarch64|x86_64)
    local boot="$ROOT/$1" label="$2" tgt="$3" host; host="$(host_arch)"
    [ -d "$boot" ] || { echo "[$label] SKIP (no $1)"; return; }

    local cc run=""
    if [ "$host" = "$tgt" ]; then
        case "$tgt" in aarch64) cc=cc ;; *) cc=gcc ;; esac
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

    # ---- CURE: default build -> [mind-pause] PASS --------------------------
    make -C "$boot" clean >/dev/null 2>&1
    if ! make -C "$boot" >/dev/null 2>&1; then
        echo "[$label] BUILD FAILED (cure)"; FAIL=1; return
    fi
    local out; out="$(run_bin "$boot/p-kernel")"
    echo "$out" | grep -E '^\[mind-pause\]'
    if echo "$out" | grep -q '^\[mind-pause\] PASS'; then
        echo "[$label] CURE PASS ([mind-pause])"
    else
        echo "[$label] CURE FAIL (expected [mind-pause] PASS)"; FAIL=1
    fi

    # ---- FALSIFIER: -DMIND_PAUSE_NO_GATE -> [mind-pause] RED ---------------
    make -C "$boot" clean >/dev/null 2>&1
    if ! make -C "$boot" EXTRA_CFLAGS=-DMIND_PAUSE_NO_GATE >/dev/null 2>&1; then
        echo "[$label] BUILD FAILED (no-gate falsifier)"; FAIL=1; return
    fi
    out="$(run_bin "$boot/p-kernel")"
    if echo "$out" | grep -q '^\[mind-pause\] FAIL'; then
        echo "[$label] FALSIFIER correctly RED ([mind-pause-NOT]: gate disabled -> HIBERNATING check fails)"
    else
        echo "[$label] FALSIFIER DID NOT go RED — [mind-pause] cert is toothless!"; FAIL=1
    fi
    make -C "$boot" clean >/dev/null 2>&1
}

crown_gate() {  # r3_incontext.c is bare-metal-linked -- prove the #ifdef held:
                # rebuild bare crowns at HEAD and diff against a clean parent
                # checkout of the same source tree (passed by the caller via
                # MIND_PAUSE_BASE_REF, e.g. the commit before this slice).
    local base="${MIND_PAUSE_BASE_REF:-}"
    if [ -z "$base" ]; then
        echo "[crown] SKIP (set MIND_PAUSE_BASE_REF=<commit before this slice> to run this gate)"; return
    fi
    for t in aarch64-linux-gnu-gcc aarch64-linux-gnu-objcopy \
             i686-linux-gnu-gcc i686-linux-gnu-objcopy; do
        command -v "$t" >/dev/null 2>&1 || { echo "[crown] SKIP (bare toolchain $t absent — the crown-text-identity CI job enforces the byte-identical invariant on every commit)"; return; }
    done
    make -C "$ROOT/boot/aarch64" clean >/dev/null 2>&1
    make -C "$ROOT/boot/x86"     clean >/dev/null 2>&1
    if ! make -C "$ROOT/boot/aarch64" >/dev/null 2>&1; then echo "[crown] aarch64 BUILD FAILED"; FAIL=1; return; fi
    if ! make -C "$ROOT/boot/x86" kernel.elf >/dev/null 2>&1; then echo "[crown] x86 BUILD FAILED"; FAIL=1; return; fi
    local a_head x_head
    a_head="$(sha256sum "$ROOT/boot/aarch64/kernel.elf" | cut -d' ' -f1)"
    x_head="$(sha256sum "$ROOT/boot/x86/kernel.elf"     | cut -d' ' -f1)"
    make -C "$ROOT/boot/aarch64" clean >/dev/null 2>&1
    make -C "$ROOT/boot/x86"     clean >/dev/null 2>&1

    local worktmp; worktmp="$(mktemp -d)"
    if ! git -C "$ROOT" worktree add --detach "$worktmp" "$base" >/dev/null 2>&1; then
        echo "[crown] SKIP (could not create a worktree at $base)"; rm -rf "$worktmp"; return
    fi
    make -C "$worktmp/boot/aarch64" clean >/dev/null 2>&1
    make -C "$worktmp/boot/x86"     clean >/dev/null 2>&1
    if ! make -C "$worktmp/boot/aarch64" >/dev/null 2>&1; then echo "[crown] aarch64 BASE BUILD FAILED"; FAIL=1
    elif ! make -C "$worktmp/boot/x86" kernel.elf >/dev/null 2>&1; then echo "[crown] x86 BASE BUILD FAILED"; FAIL=1
    else
        local a_base x_base
        a_base="$(sha256sum "$worktmp/boot/aarch64/kernel.elf" | cut -d' ' -f1)"
        x_base="$(sha256sum "$worktmp/boot/x86/kernel.elf"     | cut -d' ' -f1)"
        if [ "$a_head" = "$a_base" ] && [ "$x_head" = "$x_base" ]; then
            echo "[crown] gate PASS (bare-metal kernel.elf byte-identical to $base on both arches)"
        else
            echo "[crown] gate FAIL — bare-metal .text moved vs $base (aarch64 head=$a_head base=$a_base; x86 head=$x_head base=$x_base)"
            FAIL=1
        fi
    fi
    make -C "$worktmp/boot/aarch64" clean >/dev/null 2>&1
    make -C "$worktmp/boot/x86"     clean >/dev/null 2>&1
    git -C "$ROOT" worktree remove --force "$worktmp" >/dev/null 2>&1
    rm -rf "$worktmp"
}

one_arch "boot/linux"        "aarch64-hosted" "aarch64"
one_arch "boot/linux_x86_64" "x86_64-hosted"  "x86_64"
crown_gate

if [ "$RAN" -eq 0 ]; then
    echo "[mind-pause] NO ARCH COULD RUN (no host toolchain/runtime for any arch)"; exit 2
fi
if [ "$FAIL" -eq 0 ]; then echo "[mind-pause] ALL PASS"; exit 0
else echo "[mind-pause] FAILURES ABOVE"; exit 1; fi
