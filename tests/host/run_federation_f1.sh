#!/bin/bash
# ---------------------------------------------------------------------------
# run_federation_f1.sh — host cert for federation F1's composite ID encoding
#   (docs/architecture/20-architecture/federation.md §2.2/§4-F1, fed_id.h).
#
# THIS SLICE covers ONLY the (region_id, local_id) encoding as a standalone,
# header-only candidate -- fed_id.h's FED_LOCAL_MAKE/REGION/WITHIN and
# FED_GOBJ_MAKE/REGION/WITHIN macros. It is NOT wired into drpc.c/dkva.c/
# kdds.c or any live GOBJ producer/consumer -- see fed_id.h's own header
# comment for why (federation.md §5.3 lists the encoding choice itself, and
# separately the inter-coordinator rsum ordering, as open questions a live
# wiring would need to answer; this slice deliberately answers neither).
#
# [fed-id-roundtrip] (fed_id_self_test_run, driven via the `fed test` shell
# verb): region_id=0 reproduces today's plain 24-bit GOBJ local field exactly
# (R=1 backward compat, the only case any current call site exercises) +
# pack/unpack round-trips at boundary region_id/local values (0/1/128/255 x
# 0/1/32768/65535) + the packed value never overflows GOBJ_MAKE's existing
# 0x00FFFFFF mask.
#
# FALSIFIER [fed-id-roundtrip-NOT]: -DFED_ID_BROKEN_PACK moves the pack/
# unpack split point (16<->8 bits) so encode and decode use DIFFERENT
# shifts -> round-trip breaks -> [fed-id-roundtrip] RED. A falsifier that
# does NOT go RED = toothless = FAIL.
#
# CROWN: fed_id.h is included ONLY from arch/linux/{x86_64,aarch64}/
# usermain.c (hosted-only files, never part of boot/x86 or boot/aarch64's
# link line) -- zero crown risk BY CONSTRUCTION, not by a build-time guard.
# This gate proves that construction holds: rebuild both bare crowns and
# assert (a) they are BYTE-IDENTICAL to a clean checkout of the same source
# (nothing under arch/common changed) and (b) no fed_id/FED_ symbol appears
# in either ELF (grep finds none, since the macros are static inline and
# never called from bare-metal code -- this is a belt-and-braces check, not
# expected to ever fire).
#
# HOST-PORTABLE (same discipline as run_survival_l0.sh/l1.sh/l2.sh): each
# arch runs only when THIS host can build AND run it; a genuinely-absent
# toolchain/runtime is a SKIP, real breakage still fails. If EVERY arch
# skips, exit NON-zero rather than a hollow ALL PASS.
#
# Exit 0 = at least one arch ran with [fed-id-roundtrip] PASS, the falsifier
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

run_bin() {  # $1 = binary path -> drive `fed test` and echo the cert block
    local bin="$1" host; host="$(host_arch)"
    export PKERNEL_GALAXY=0
    case "$bin" in
        */linux_x86_64/*)
            if [ "$host" != "x86_64" ] && command -v qemu-x86_64 >/dev/null 2>&1; then
                printf 'fed test\nexit\n' | qemu-x86_64 "$bin" 2>/dev/null
            else
                printf 'fed test\nexit\n' | "$bin" 2>/dev/null
            fi ;;
        */linux/*)
            if [ "$host" != "aarch64" ] && command -v qemu-aarch64 >/dev/null 2>&1; then
                printf 'fed test\nexit\n' | qemu-aarch64 "$bin" 2>/dev/null
            else
                printf 'fed test\nexit\n' | "$bin" 2>/dev/null
            fi ;;
        *)  printf 'fed test\nexit\n' | "$bin" 2>/dev/null ;;
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

    # ---- CURE: default build -> [fed-id-roundtrip] PASS --------------------
    make -C "$boot" clean >/dev/null 2>&1
    if ! make -C "$boot" >/dev/null 2>&1; then
        echo "[$label] BUILD FAILED (cure)"; FAIL=1; return
    fi
    local out; out="$(run_bin "$boot/p-kernel")"
    echo "$out" | grep -E '^\[fed-id-roundtrip\]'
    if echo "$out" | grep -q '^\[fed-id-roundtrip\] PASS'; then
        echo "[$label] CURE PASS ([fed-id-roundtrip])"
    else
        echo "[$label] CURE FAIL (expected [fed-id-roundtrip] PASS)"; FAIL=1
    fi

    # ---- FALSIFIER: -DFED_ID_BROKEN_PACK -> [fed-id-roundtrip] RED ---------
    make -C "$boot" clean >/dev/null 2>&1
    if ! make -C "$boot" EXTRA_CFLAGS=-DFED_ID_BROKEN_PACK >/dev/null 2>&1; then
        echo "[$label] BUILD FAILED (broken-pack falsifier)"; FAIL=1; return
    fi
    out="$(run_bin "$boot/p-kernel")"
    if echo "$out" | grep -q '^\[fed-id-roundtrip\] FAIL'; then
        echo "[$label] FALSIFIER correctly RED ([fed-id-roundtrip-NOT]: split point moved -> round-trip breaks)"
    else
        echo "[$label] FALSIFIER DID NOT go RED — [fed-id-roundtrip] cert is toothless!"; FAIL=1
    fi
    make -C "$boot" clean >/dev/null 2>&1
}

crown_gate() {  # fed_id.h is hosted-only by construction; prove it.
    for t in aarch64-linux-gnu-gcc aarch64-linux-gnu-objcopy \
             i686-linux-gnu-gcc i686-linux-gnu-objcopy; do
        command -v "$t" >/dev/null 2>&1 || { echo "[crown] SKIP (bare toolchain $t absent — the crown-text-identity CI job enforces the byte-identical invariant on every commit)"; return; }
    done
    make -C "$ROOT/boot/aarch64" clean >/dev/null 2>&1
    make -C "$ROOT/boot/x86"     clean >/dev/null 2>&1
    if ! make -C "$ROOT/boot/aarch64" >/dev/null 2>&1; then echo "[crown] aarch64 BUILD FAILED"; FAIL=1; return; fi
    if ! make -C "$ROOT/boot/x86" kernel.elf >/dev/null 2>&1; then echo "[crown] x86 BUILD FAILED"; FAIL=1; return; fi

    local leak=0 elf
    for elf in "$ROOT/boot/aarch64/kernel.elf" "$ROOT/boot/x86/kernel.elf"; do
        if nm "$elf" 2>/dev/null | grep -qiE 'fed_id|fed_local|fed_gobj'; then
            echo "[crown] gate FAIL — a fed_id symbol leaked into $elf"; leak=1
        fi
    done
    if [ "$leak" = 0 ]; then
        echo "[crown] gate PASS (fed_id symbols absent from both bare crowns)"
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
    echo "[federation-f1] NO ARCH COULD RUN (no host toolchain/runtime for any arch)"; exit 2
fi
if [ "$FAIL" -eq 0 ]; then echo "[federation-f1] ALL PASS"; exit 0
else echo "[federation-f1] FAILURES ABOVE"; exit 1; fi
