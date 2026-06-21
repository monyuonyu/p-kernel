#!/bin/sh
# tests/aarch64/run_smp0.sh
#
# ②.0 full-SMP cert harness (docs/architecture/full-smp-plan.md §7 ②.0).
#
# Builds the aarch64 bare-metal kernel WITH the ②.0 SMP self-test
# (-DSMP_SELFTEST), boots it under QEMU virt with -smp 4 (uses 2 CPUs:
# the boot CPU + ONE secondary released into the per-CPU T-Kernel
# dispatcher under one Big Kernel Lock), captures the UART, and asserts:
#
#   [smp-2-tasks-run]      SMP-RUN: PASS    — both CPUs advanced their OWN
#                          per-CPU task (cpu0 & cpu1 exec_count>0, distinct
#                          per-CPU current tasks).
#   [smp-mutual-exclusion] SMP-MUTEX: PASS  — a shared counter incremented
#                          by tasks on BOTH CPUs under the BKL reaches the
#                          EXACT expected total (no lost updates).
#   [smp-boot-survives]    SMP-BOOT: PASS + the T-Kernel still boots
#                          ([BOOT] Starting T-Kernel.../Initial task started)
#                          AFTER both CPUs ran the dispatcher (no deadlock).
#
# Then the LOAD-BEARING falsifier: rebuild with -DSMP_MUTEX_NOLOCK so the
# shared-counter increment BYPASSES the BKL. Under -smp with truly
# concurrent tasks the RMW races -> lost updates -> SMP-MUTEX: FAIL with a
# total < expected. This proves the lock is load-bearing (the cert is NOT
# vacuously passing).
#
# HONESTY (MC-2 §4.4, inherited): QEMU TCG models memory strongly and may
# MASK a missing-barrier/SMPEN race; the BKL's barrier teeth are only fully
# [live] on RPi3 hardware. A QEMU green proves "the lock serializes + 2
# CPUs run distinct tasks", NOT the barrier discipline on weak silicon.
#
# Exit 0 = cert PASS; non-zero = FAIL. Greps the UART for the verdicts.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
BOOT="$HERE/../../boot/aarch64"
QEMU="qemu-system-aarch64"
TIMEOUT_S="${SMP0_TIMEOUT:-60}"

QEMU_SMP_FLAGS="-M virt -cpu cortex-a53 -smp 4 -m 256M -kernel kernel.elf \
                -serial stdio -display none -no-reboot"

fail() { echo "[smp0] FAIL: $*" >&2; exit 1; }

run_qemu() {
    # $1 = log file. The kernel boots into the T-Kernel shell and never
    # exits on its own, so the wall-clock timeout is the expected
    # terminator; we only care about the UART captured before it.
    log="$1"
    cd "$BOOT"
    if command -v timeout >/dev/null 2>&1; then
        timeout -k 3 "$TIMEOUT_S" $QEMU $QEMU_SMP_FLAGS >"$log" 2>&1 || true
    else
        $QEMU $QEMU_SMP_FLAGS >"$log" 2>&1 &
        qpid=$!
        sleep "$TIMEOUT_S"
        kill "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
}

# ── 1. The cert build: SMP-RUN / SMP-MUTEX / SMP-BOOT all PASS ──────────
echo "=== ②.0 [smp-2-tasks-run]+[smp-mutual-exclusion]+[smp-boot-survives] under -smp 4 ==="
cd "$BOOT"
make clean >/dev/null 2>&1
make EXTRA_CFLAGS=-DSMP_SELFTEST >/dev/null 2>&1 \
    || fail "build (SMP_SELFTEST) failed"

PASS_LOG="$(mktemp)"
run_qemu "$PASS_LOG"
echo "----- captured UART (SMP cert build) -----"
cat "$PASS_LOG"
echo "------------------------------------------"

grep -q "cpu1 entered dispatcher" "$PASS_LOG" \
    || fail "secondary CPU never entered the per-CPU dispatcher"
grep -q "SMP-RUN: PASS" "$PASS_LOG" \
    || fail "no 'SMP-RUN: PASS' (2 CPUs did not run distinct tasks)"
grep -q "SMP-MUTEX: PASS" "$PASS_LOG" \
    || fail "no 'SMP-MUTEX: PASS' (BKL did not serialize the shared counter)"
grep -q "SMP-BOOT: PASS" "$PASS_LOG" \
    || fail "no 'SMP-BOOT: PASS' (a CPU wedged or the join timed out)"
grep -q "Starting T-Kernel" "$PASS_LOG" \
    || fail "kernel did not reach the T-Kernel banner after the SMP slice"
grep -q "Initial task started" "$PASS_LOG" \
    || fail "T-Kernel scheduler did not tick after the SMP slice"
echo "[smp0] cert build: PASS (SMP-RUN + SMP-MUTEX + SMP-BOOT, T-Kernel still boots)"
echo

# ── 2. The load-bearing falsifier: NOLOCK -> SMP-MUTEX: FAIL ────────────
echo "=== ②.0 falsifier: -DSMP_MUTEX_NOLOCK MUST produce SMP-MUTEX: FAIL ==="
make clean >/dev/null 2>&1
make EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_MUTEX_NOLOCK" >/dev/null 2>&1 \
    || fail "build (SMP_MUTEX_NOLOCK) failed"

NOLOCK_LOG="$(mktemp)"
run_qemu "$NOLOCK_LOG"
echo "----- captured UART (NOLOCK falsifier) -----"
cat "$NOLOCK_LOG"
echo "--------------------------------------------"

grep -q "SMP-MUTEX: FAIL" "$NOLOCK_LOG" \
    || fail "NOLOCK falsifier did NOT lose updates (BKL not load-bearing?!)"
# The kernel must still survive the falsifier (it only loses counter
# updates; it must not crash/hang) and still boot the T-Kernel.
grep -q "Starting T-Kernel" "$NOLOCK_LOG" \
    || fail "NOLOCK falsifier wedged the primary (never reached T-Kernel)"
echo "[smp0] falsifier: PASS (BKL bypass -> lost updates -> SMP-MUTEX: FAIL, as required)"
echo

rm -f "$PASS_LOG" "$NOLOCK_LOG"
echo "=== ②.0 full-SMP : ALL PASS ==="
