#!/bin/sh
# tests/aarch64/run_mc2_slice.sh
#
# MC-2.1b — the STANDALONE pk_slice_bm partition unit-check harness.
#
# The MC-2.1a equiv cert (run_mc2_equiv.sh) exercises the hand-copied
# partition function pk_slice_bm (arch/aarch64/mc2_smp.c) only INDIRECTLY:
# the matmul reassembles byte-identically => the partition was sound. The
# MC-2.1a audit flagged that as a drift surface — the hand-copy of pk_slice()
# (arch/common/llm/pk_parallel.c:57-63, the "one mind, one math" golden) had
# no DIRECT guard. This harness adds one.
#
# It builds the aarch64 bare-metal kernel WITH the standalone unit-check
# (-DMC2_SLICE_SELFTEST) and boots it under QEMU virt. The check is PURE
# INTEGER — it needs NO secondary cores (it runs on the primary alone before
# any matmul), so this boots WITHOUT -smp (cheap). It asserts:
#
#   1. MC2-SLICE: PASS         (pk_slice_bm satisfies, for a representative
#                               set of (out,nw) pairs incl. ragged ones, the
#                               three partition invariants DIRECTLY:
#                               DISJOINT+TOTAL, ORDER, MATCHES-THE-GOLDEN)
#   2. [BOOT] Starting T-Kernel...    (the kernel still boots after the check)
#   3. [T-Kernel] Initial task started (the scheduler ticks — no regression)
#
# Then it builds + boots the FALSIFIER (-DMC2_SLICE_BREAK), which perturbs
# pk_slice_bm itself (drops the ragged remainder) and MUST produce
# MC2-SLICE: FAIL — the load-bearing teeth that prove the check would catch a
# real drift of the hand-copy (a "one mind, one math" violation).
#
# Exit 0 = cert PASS; non-zero = FAIL. Greps the UART for the verdict lines;
# nothing here is paper. Keeps -O1 -ffp-contract=off (the Makefile CFLAGS).
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
BOOT="$HERE/../../boot/aarch64"
QEMU="qemu-system-aarch64"
TIMEOUT_S="${MC2_TIMEOUT:-30}"

# No -smp: the unit-check is a pure integer function on the primary core.
QEMU_FLAGS="-M virt -cpu cortex-a53 -m 256M -kernel kernel.elf \
            -serial stdio -display none -no-reboot"

fail() { echo "[mc2-slice] FAIL: $*" >&2; exit 1; }

run_qemu() {
    # $1 = log file. The kernel boots into the T-Kernel idle loop and never
    # exits on its own, so the wall-clock timeout is the expected terminator.
    log="$1"
    cd "$BOOT"
    if command -v timeout >/dev/null 2>&1; then
        timeout -k 3 "$TIMEOUT_S" $QEMU $QEMU_FLAGS >"$log" 2>&1 || true
    else
        $QEMU $QEMU_FLAGS >"$log" 2>&1 &
        qpid=$!
        sleep "$TIMEOUT_S"
        kill "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
}

# -------------------------------------------------------------------------
echo "=== [mc2-slice] : standalone pk_slice_bm partition unit-check ==="
cd "$BOOT"
make clean >/dev/null 2>&1
make EXTRA_CFLAGS="-DMC2_SLICE_SELFTEST" >/dev/null 2>&1 \
    || fail "build (MC2_SLICE_SELFTEST) failed"

SLICE_LOG="$(mktemp)"
run_qemu "$SLICE_LOG"
echo "----- captured UART (slice check) -----"
cat "$SLICE_LOG"
echo "---------------------------------------"

grep -q "MC2-SLICE: PASS" "$SLICE_LOG" \
    || fail "no 'MC2-SLICE: PASS' in UART (pk_slice_bm violated a partition invariant)"
grep -q "Starting T-Kernel" "$SLICE_LOG" \
    || fail "kernel did not reach the T-Kernel banner after the slice check"
grep -q "Initial task started" "$SLICE_LOG" \
    || fail "T-Kernel scheduler did not tick after the slice check"
echo "[mc2-slice] partition invariants: PASS"
echo

# -------------------------------------------------------------------------
echo "=== [mc2-slice] : FALSIFIER (-DMC2_SLICE_BREAK) MUST FAIL ==="
make clean >/dev/null 2>&1
make EXTRA_CFLAGS="-DMC2_SLICE_SELFTEST -DMC2_SLICE_BREAK" >/dev/null 2>&1 \
    || fail "build (MC2_SLICE_BREAK) failed"

BREAK_LOG="$(mktemp)"
run_qemu "$BREAK_LOG"
echo "----- captured UART (falsifier) -----"
cat "$BREAK_LOG"
echo "-------------------------------------"

grep -q "MC2-SLICE: FAIL" "$BREAK_LOG" \
    || fail "falsifier (dropped ragged remainder) did NOT fail — the check is VACUOUS!"
grep -q "MC2-SLICE: PASS" "$BREAK_LOG" \
    && fail "falsifier produced a PASS — the unit-check has no teeth"
echo "[mc2-slice] falsifier: correctly FAILED (the check has teeth)"
echo

rm -f "$SLICE_LOG" "$BREAK_LOG"
echo "=== [mc2-slice] : PASS (partition invariants hold; falsifier bites) ==="
