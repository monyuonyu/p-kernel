#!/bin/sh
# tests/aarch64/run_mc2_equiv.sh
#
# [mc2-smp-equiv] cert harness (docs/architecture/
# mc2-1-ncore-equiv-plan.md §4).
#
# Builds the aarch64 bare-metal kernel WITH the MC-2.1 N-core equivalence
# self-test (-DMC2_EQUIV_SELFTEST), boots it under QEMU
# virt with -smp 4 (so cores 1,2,3 actually exist in the VM), captures the
# UART, and asserts the deterministic-worker MECHANISM:
#
#   1. MC2-EQUIV: PASS                  (a synthetic gate-exceeding matmul
#                                        computed via the bare-metal
#                                        pk_parallel_rows is BYTE-IDENTICAL
#                                        to the serial loop for nw in {1,2,4})
#   2. the three FNV-1a hashes (nw=1,2,4) printed by the kernel are IDENTICAL
#   3. [BOOT] Starting T-Kernel...      (the primary still boots after the run)
#   4. p-kernel>  shell prompt  (the scheduler actually ticks; the aarch64 initial
#      task ran to the prompt. NOT the x86-only "Initial task started" phantom.)
#
# Then it builds + boots TOOTH A (the partition/arithmetic falsifier,
# -DMC2_EQUIV_RACY_PARTITION) and asserts it FAILS (MC2-EQUIV: FAIL) under
# -smp 4 — this is the load-bearing falsifier that proves the cert is NOT
# vacuous (pure arithmetic -> QEMU TCG catches it).
#
# Then it builds + boots TOOTH B (the barrier/SMPEN falsifier,
# -DMC2_EQUIV_SMPEN_OFF -DMC2_EQUIV_NO_BARRIER) and records its QEMU result
# HONESTLY: if it still PASSes that is EXPECTED (TCG masks the memory-
# ordering race) and recorded as "masked, deferred to RPi3 (MC-2.2)" — NOT
# a cert failure. Tooth B is only fully load-bearing [live] on real RPi3.
#
# Exit 0 = cert PASS; non-zero = FAIL. Greps the UART for the verdict
# lines; nothing here is paper.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
BOOT="$HERE/../../boot/aarch64"
QEMU="qemu-system-aarch64"
TIMEOUT_S="${MC2_TIMEOUT:-60}"

# NOTE (②.1b harness-snapshot fix): this harness does THREE build+boot cycles
# (equiv, Tooth A, Tooth B). Boot QEMU from a UNIQUE snapshot of kernel.elf
# (run_qemu $2), not the live build-dir image, so each later `make` (or a
# parallel auditor's) cannot corrupt an in-flight QEMU read.
QEMU_SMP_FLAGS="-M virt -cpu cortex-a53 -smp 4 -m 256M \
                -serial stdio -display none -no-reboot"

fail() { echo "[mc2-smp-equiv] FAIL: $*" >&2; exit 1; }

run_qemu() {
    # $1 = log file, $2 = kernel image (a unique snapshot). Boots with a hard
    # wall-clock timeout; the kernel never exits on its own (it boots into the
    # T-Kernel idle loop), so the timeout is the expected terminator.
    log="$1"; img="$2"
    cd "$BOOT"
    if command -v timeout >/dev/null 2>&1; then
        timeout -k 3 "$TIMEOUT_S" $QEMU $QEMU_SMP_FLAGS -kernel "$img" \
            >"$log" 2>&1 || true
    else
        $QEMU $QEMU_SMP_FLAGS -kernel "$img" >"$log" 2>&1 &
        qpid=$!
        sleep "$TIMEOUT_S"
        kill "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
}

# Extract the three FNV hashes (nw=1,2,4) and assert they are identical.
assert_hashes_equal() {
    log="$1"
    h1="$(grep -E 'FNV nw=1 ' "$log" | sed -n 's/.*FNV nw=1 //p' | tr -d '\r' | head -1)"
    h2="$(grep -E 'FNV nw=2 ' "$log" | sed -n 's/.*FNV nw=2 //p' | tr -d '\r' | head -1)"
    h4="$(grep -E 'FNV nw=4 ' "$log" | sed -n 's/.*FNV nw=4 //p' | tr -d '\r' | head -1)"
    echo "[mc2-smp-equiv] FNV hashes: nw=1 $h1 | nw=2 $h2 | nw=4 $h4"
    [ -n "$h1" ] && [ -n "$h2" ] && [ -n "$h4" ] \
        || fail "missing one of the FNV hashes (nw=1/2/4)"
    [ "$h1" = "$h2" ] && [ "$h2" = "$h4" ] \
        || fail "FNV hashes differ across nw (split mind!): $h1 / $h2 / $h4"
    echo "[mc2-smp-equiv] all three FNV hashes IDENTICAL: $h1"
}

# -------------------------------------------------------------------------
echo "=== [mc2-smp-equiv] : byte-identity across nw in {1,2,4} under -smp 4 ==="
cd "$BOOT"
make clean >/dev/null 2>&1
make EXTRA_CFLAGS="-DMC2_EQUIV_SELFTEST" >/dev/null 2>&1 \
    || fail "build (MC2_EQUIV_SELFTEST) failed"
EQUIV_IMG="$(mktemp /tmp/mc2equiv_main.XXXXXX.elf)"
cp kernel.elf "$EQUIV_IMG"       # snapshot so a later rebuild can't corrupt it

EQUIV_LOG="$(mktemp)"
run_qemu "$EQUIV_LOG" "$EQUIV_IMG"
echo "----- captured UART (equiv) -----"
cat "$EQUIV_LOG"
echo "---------------------------------"

grep -q "MC2-EQUIV: PASS" "$EQUIV_LOG" \
    || fail "no 'MC2-EQUIV: PASS' in UART (matmul NOT byte-identical / core failed to wake)"
assert_hashes_equal "$EQUIV_LOG"
grep -q "Starting T-Kernel" "$EQUIV_LOG" \
    || fail "kernel did not reach the T-Kernel banner after the equiv run"
grep -q "p-kernel>" "$EQUIV_LOG" \
    || fail "T-Kernel init task did not reach the shell prompt after the equiv run"
echo "[mc2-smp-equiv] byte-identity: PASS"
echo

# -------------------------------------------------------------------------
echo "=== [mc2-smp-equiv] : TOOTH A (partition/arithmetic falsifier) MUST FAIL ==="
make clean >/dev/null 2>&1
make EXTRA_CFLAGS="-DMC2_EQUIV_SELFTEST -DMC2_EQUIV_RACY_PARTITION" \
    >/dev/null 2>&1 || fail "build (MC2_EQUIV_RACY_PARTITION) failed"
TOOTHA_IMG="$(mktemp /tmp/mc2equiv_toothA.XXXXXX.elf)"
cp kernel.elf "$TOOTHA_IMG"      # snapshot so a later rebuild can't corrupt it

TOOTHA_LOG="$(mktemp)"
run_qemu "$TOOTHA_LOG" "$TOOTHA_IMG"
echo "----- captured UART (Tooth A) -----"
cat "$TOOTHA_LOG"
echo "-----------------------------------"

grep -q "MC2-EQUIV: FAIL" "$TOOTHA_LOG" \
    || fail "Tooth A (reassociated reduction) did NOT fail — the cert is VACUOUS!"
grep -q "MC2-EQUIV: PASS" "$TOOTHA_LOG" \
    && fail "Tooth A produced a PASS — falsifier ineffective"
echo "[mc2-smp-equiv] Tooth A: correctly FAILED (cert has teeth)"
echo

# -------------------------------------------------------------------------
echo "=== [mc2-smp-equiv] : TOOTH B (barrier/SMPEN falsifier) — honest QEMU record ==="
make clean >/dev/null 2>&1
make EXTRA_CFLAGS="-DMC2_EQUIV_SELFTEST -DMC2_EQUIV_SMPEN_OFF -DMC2_EQUIV_NO_BARRIER" \
    >/dev/null 2>&1 || fail "build (Tooth B) failed"
TOOTHB_IMG="$(mktemp /tmp/mc2equiv_toothB.XXXXXX.elf)"
cp kernel.elf "$TOOTHB_IMG"      # snapshot so a later rebuild can't corrupt it

TOOTHB_LOG="$(mktemp)"
run_qemu "$TOOTHB_LOG" "$TOOTHB_IMG"
echo "----- captured UART (Tooth B) -----"
cat "$TOOTHB_LOG"
echo "-----------------------------------"

if grep -q "MC2-EQUIV: FAIL" "$TOOTHB_LOG"; then
    echo "[mc2-smp-equiv] Tooth B: FAILED on QEMU (bonus — TCG happened to expose the race)"
elif grep -q "MC2-EQUIV: PASS" "$TOOTHB_LOG"; then
    echo "[mc2-smp-equiv] Tooth B: PASSED on QEMU — EXPECTED & MASKED."
    echo "[mc2-smp-equiv]   QEMU TCG models memory strongly and does NOT reproduce the"
    echo "[mc2-smp-equiv]   missing-barrier / SMPEN=0 race a physical Cortex-A53 exhibits."
    echo "[mc2-smp-equiv]   The barrier/SMPEN sub-claim (O4) is [live]-DEFERRED to RPi3 (MC-2.2)."
    echo "[mc2-smp-equiv]   This is NOT a cert failure and does NOT mean 'barriers verified'."
else
    echo "[mc2-smp-equiv] Tooth B: no verdict line (kernel did not reach the equiv block)"
fi
echo

rm -f "$EQUIV_LOG" "$TOOTHA_LOG" "$TOOTHB_LOG" \
      "$EQUIV_IMG" "$TOOTHA_IMG" "$TOOTHB_IMG"
echo "=== [mc2-smp-equiv] : PASS (byte-identity + Tooth A bites; Tooth B recorded honestly) ==="
