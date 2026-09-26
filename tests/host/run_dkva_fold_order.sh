#!/bin/bash
# ---------------------------------------------------------------------------
# run_dkva_fold_order.sh — host cert for federation.md §5.3-2 (settled
#   2026-09-27, inbox #3): the requester folds the partials / region summaries
#   it received in a FIXED order (self, then node id ascending; resp before rsum
#   at each id), so the same set of contributions gives a byte-identical result
#   whatever order they arrived in.
#
# [dkva-fold-order] (dkva_fold_order_test, run by `dkva test`): feeds one set of
# contributions through the production fold_arrive/fold_finish in two arrival
# orders (id-ascending and reversed) and checks F1 the two results are
# byte-identical, F2 they equal the id-order reference sum, F3 the reference
# really is order-sensitive (1e8 + 1 - 1e8 + 1 + 0.5 = 1.5 in float).
#
# FALSIFIER: -DDKVA_FOLD_ARRIVAL_ORDER restores the old fold-on-arrival; the
# reversed order then sums to 0, F1/F2 go RED. A falsifier that does NOT go
# RED = toothless = FAIL.
#
# CROWN: dkva.c links into both bare-metal kernels, so this change MOVES the
# crown. That is expected and goes through re-bless; this script does not gate
# it (the crown-text-identity CI job does).
#
# HOST-PORTABLE like run_mind_pause.sh: an arch runs only when this host can
# build AND run it; absent toolchain = SKIP; if every arch skips, exit 2.
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
    esac | grep -a '^\[dkva-fold-order\]'
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
    if echo "$out" | grep -q '^\[dkva-fold-order\] PASS'; then
        echo "[$label] CURE PASS ([dkva-fold-order])"
    else
        echo "[$label] CURE FAIL (expected [dkva-fold-order] PASS)"; FAIL=1
    fi

    make -C "$boot" clean >/dev/null 2>&1
    make -C "$boot" EXTRA_CFLAGS=-DDKVA_FOLD_ARRIVAL_ORDER >/dev/null 2>&1 \
        || { echo "[$label] BUILD FAILED (arrival-order falsifier)"; FAIL=1; return; }
    out="$(run_bin "$boot/p-kernel")"
    echo "$out" | sed 's/^/  falsifier: /'
    if echo "$out" | grep -q '^\[dkva-fold-order\] FAIL'; then
        echo "[$label] FALSIFIER correctly RED (arrival-order fold depends on arrival order)"
    else
        echo "[$label] FALSIFIER DID NOT go RED — [dkva-fold-order] is toothless!"; FAIL=1
    fi
    make -C "$boot" clean >/dev/null 2>&1
}

one_arch "boot/linux"        "aarch64-hosted" "aarch64"
one_arch "boot/linux_x86_64" "x86_64-hosted"  "x86_64"

if [ "$RAN" -eq 0 ]; then
    echo "[dkva-fold-order] NO ARCH COULD RUN (no host toolchain/runtime for any arch)"; exit 2
fi
if [ "$FAIL" -eq 0 ]; then echo "[dkva-fold-order] ALL PASS"; exit 0
else echo "[dkva-fold-order] FAILURES ABOVE"; exit 1; fi
