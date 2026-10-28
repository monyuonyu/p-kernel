#!/bin/bash
# ---------------------------------------------------------------------------
# run_dkva_resp_to.sh — [dkva-resp-to] (ROADMAP N1, research/n1-fleet-64.md).
#   A responder sends its dkva answer for origin o only to the nodes that read
#   resp/<me> for o: the origin when it is in my region, and my region
#   coordinator (cagg) when that is neither me nor the origin. Not to the whole
#   region (64 nodes: 63 copies x 100 re-sends per answer).
#
# dkva_resp_to_test (run by `dkva test`) checks the destination table of
# dkva_resp_dests, R1-R6. FALSIFIER: -DDKVA_RESP_REGION_ALL restores the old
# "whole region" send; the table then goes RED. Toothless falsifier = FAIL.
#
# CROWN: dkva.c and kdds.c link into both bare-metal kernels = the crown moves
# (re-bless). Same harness form as run_dkva_fold_order.sh.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FAIL=0
RAN=0

host_arch() { case "$(uname -m)" in arm64) echo aarch64 ;; *) uname -m ;; esac; }

run_bin() {  # $1 = binary -> drive `dkva test`, print the fold-order block
    local bin="$1" host; host="$(host_arch)"
    export PKERNEL_GALAXY=0
    case "$bin" in
        */linux_x86_64/*)
            if [ "$host" != "x86_64" ]; then
                printf 'dkva test\nexit\n' | qemu-x86_64 "$bin" 2>/dev/null
            else printf 'dkva test\nexit\n' | "$bin" 2>/dev/null; fi ;;
        *)
            if [ "$host" != "aarch64" ]; then
                printf 'dkva test\nexit\n' | qemu-aarch64 "$bin" 2>/dev/null
            else printf 'dkva test\nexit\n' | "$bin" 2>/dev/null; fi ;;
    esac | grep -a '^\[dkva-resp-to\]'
}

one_arch() {  # $1 = boot dir, $2 = label, $3 = target arch
    local boot="$ROOT/$1" label="$2" tgt="$3" host cc run=""; host="$(host_arch)"
    [ -d "$boot" ] || { echo "[$label] SKIP (no $1)"; return; }
    if [ "$host" = "$tgt" ]; then
        case "$tgt" in aarch64) cc=cc ;; *) cc=gcc ;; esac
    else
        cc="${tgt}-linux-gnu-gcc"; run="qemu-${tgt}"
    fi
    command -v "$cc" >/dev/null 2>&1 || { echo "[$label] SKIP (toolchain/runtime absent)"; return; }
    if [ -n "$run" ] && ! command -v "$run" >/dev/null 2>&1; then
        echo "[$label] SKIP (toolchain/runtime absent)"; return
    fi
    RAN=1

    make -C "$boot" clean >/dev/null 2>&1
    make -C "$boot" >/dev/null 2>&1 || { echo "[$label] BUILD FAILED (cure)"; FAIL=1; return; }
    local out; out="$(run_bin "$boot/p-kernel")"
    echo "$out"
    if echo "$out" | grep -q '^\[dkva-resp-to\] PASS'; then
        echo "[$label] CURE PASS ([dkva-resp-to])"
    else
        echo "[$label] CURE FAIL (expected [dkva-resp-to] PASS)"; FAIL=1
    fi

    make -C "$boot" clean >/dev/null 2>&1
    make -C "$boot" EXTRA_CFLAGS=-DDKVA_RESP_REGION_ALL >/dev/null 2>&1 \
        || { echo "[$label] BUILD FAILED (region-all falsifier)"; FAIL=1; return; }
    out="$(run_bin "$boot/p-kernel")"
    echo "$out" | sed 's/^/  falsifier: /'
    if echo "$out" | grep -q '^\[dkva-resp-to\] FAIL'; then
        echo "[$label] FALSIFIER correctly RED (region-all sends every answer to the whole region)"
    else
        echo "[$label] FALSIFIER DID NOT go RED — [dkva-resp-to] is toothless!"; FAIL=1
    fi
    make -C "$boot" clean >/dev/null 2>&1
}

one_arch "boot/linux"        "aarch64-hosted" "aarch64"
one_arch "boot/linux_x86_64" "x86_64-hosted"  "x86_64"

if [ "$RAN" -eq 0 ]; then
    echo "[dkva-resp-to] NO ARCH COULD RUN (no host toolchain/runtime for any arch)"; exit 2
fi
if [ "$FAIL" -eq 0 ]; then echo "[dkva-resp-to] ALL PASS"; exit 0
else echo "[dkva-resp-to] FAILURES ABOVE"; exit 1; fi
