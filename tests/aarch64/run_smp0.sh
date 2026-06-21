#!/bin/sh
# tests/aarch64/run_smp0.sh
#
# ②.0/②.N8 full-SMP cert harness (docs/architecture/full-smp-plan.md §7 ②.0).
#
# Builds the aarch64 bare-metal kernel WITH the ②.0/②.1b/②.N8 SMP self-test
# (-DSMP_SELFTEST), boots it under QEMU virt with -smp 8 (②.N8 uses ALL 8
# CPUs: the boot CPU + SEVEN secondaries released into the per-CPU T-Kernel
# dispatcher under one Big Kernel Lock — the real phone target, the GICv2
# 8-CPU ceiling), captures the UART, and asserts:
#
#   [smp-2-tasks-run]      SMP-RUN: PASS    — ALL EIGHT CPUs advanced their OWN
#                          per-CPU task (cpu0..7 exec_count>0, eight distinct
#                          per-CPU current tasks).
#   [smp-mutual-exclusion] SMP-MUTEX: PASS  — a shared counter incremented
#                          by tasks on ALL EIGHT CPUs under the BKL reaches the
#                          EXACT expected total N*K (N=8 => 1600000; no lost
#                          updates).
#   [smp-boot-survives]    SMP-BOOT: PASS + the T-Kernel still boots
#                          ([BOOT] Starting T-Kernel.../Initial task started)
#                          AFTER all 8 CPUs ran the dispatcher (no deadlock).
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

# NOTE (②.1b harness-snapshot fix): the -kernel image is passed PER RUN as a
# unique snapshot path (run_qemu $2), NOT the live build-dir kernel.elf — so a
# CONCURRENT rebuild (e.g. a parallel auditor's `make`, or this harness's own
# falsifier build) cannot corrupt an in-flight QEMU read. The ②.0 audit had to
# exonerate exactly such a self-inflicted flake by timestamp; snapshotting
# removes the race at the source.
QEMU_SMP_FLAGS="-M virt -cpu cortex-a53 -smp 8 -m 256M \
                -serial stdio -display none -no-reboot"

fail() { echo "[smp0] FAIL: $*" >&2; exit 1; }

run_qemu() {
    # $1 = log file, $2 = kernel image (a unique snapshot). The kernel boots
    # into the T-Kernel shell and never exits on its own, so the wall-clock
    # timeout is the expected terminator; we only care about the UART
    # captured before it.
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

# ── 1. The cert build: SMP-RUN / SMP-MUTEX / SMP-BOOT all PASS ──────────
echo "=== ②.N8 [smp-2-tasks-run]+[smp-mutual-exclusion]+[smp-boot-survives] under -smp 8 ==="
cd "$BOOT"
make clean >/dev/null 2>&1
make EXTRA_CFLAGS=-DSMP_SELFTEST >/dev/null 2>&1 \
    || fail "build (SMP_SELFTEST) failed"
PASS_IMG="$(mktemp /tmp/smp0_pass.XXXXXX.elf)"
cp kernel.elf "$PASS_IMG"        # snapshot so a later rebuild can't corrupt it

PASS_LOG="$(mktemp)"
run_qemu "$PASS_LOG" "$PASS_IMG"
echo "----- captured UART (SMP cert build) -----"
cat "$PASS_LOG"
echo "------------------------------------------"

grep -q "cpu1 entered dispatcher" "$PASS_LOG" \
    || fail "secondary CPU never entered the per-CPU dispatcher"
# ②.N8: also assert the HIGHEST secondary (cpu7) woke — proves all SEVEN
# secondaries (cores 1..7) reached the dispatcher, not just cpu1.
grep -q "cpu7 entered dispatcher" "$PASS_LOG" \
    || fail "highest secondary cpu7 never entered the dispatcher (not all 7 woke)"
grep -q "SMP-RUN: PASS" "$PASS_LOG" \
    || fail "no 'SMP-RUN: PASS' (8 CPUs did not run distinct tasks)"
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
NOLOCK_IMG="$(mktemp /tmp/smp0_nolock.XXXXXX.elf)"
cp kernel.elf "$NOLOCK_IMG"      # snapshot so a later rebuild can't corrupt it

NOLOCK_LOG="$(mktemp)"
run_qemu "$NOLOCK_LOG" "$NOLOCK_IMG"
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

rm -f "$PASS_LOG" "$NOLOCK_LOG" "$PASS_IMG" "$NOLOCK_IMG"
echo "=== ②.0 full-SMP : ALL PASS ==="
