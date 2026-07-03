#!/bin/sh
# tests/aarch64/run_smp0.sh
#
# ②.0/②.N8 full-SMP cert harness + [smp-autodetect] (slice 1 of
# docs/architecture/device-autodetect-plan.md).
#
# Builds the aarch64 bare-metal kernel WITH the SMP self-test (-DSMP_SELFTEST)
# ONCE, then boots that SAME binary under QEMU virt with -smp 2, -smp 4, AND
# -smp 8. The kernel AUTODETECTS the core count at boot via GICD_TYPER bits[7:5]
# ((TYPER>>5)&0x7)+1) and wakes EXACTLY that many secondaries — proving the
# SAME binary adapts to any device with no recompile («デバイスのスペックを
# 測って自動で合わしたい»). For each -smp N it asserts:
#
#   [smp-autodetect]       "[SMP] detected N cpus via GICD_TYPER" — the detect
#                          read the real interface count.
#   [smp-2-tasks-run]      SMP-RUN: PASS — ALL N woken CPUs advanced their OWN
#                          per-CPU task (cpu0..N-1 exec_count>0, N distinct
#                          per-CPU current tasks).
#   [smp-mutual-exclusion] SMP-MUTEX: PASS — a shared counter incremented by
#                          tasks on ALL N CPUs under the BKL reaches the EXACT
#                          expected total N*K (K=200000): -smp 2 => 400000,
#                          -smp 4 => 800000, -smp 8 => 1600000. No lost updates.
#   [smp-boot-survives]    SMP-BOOT: PASS + the T-Kernel still boots
#                          ([BOOT] Starting T-Kernel.../Initial task started)
#                          AFTER all N CPUs ran the dispatcher (no deadlock).
#
# 8 is the GICv2 ceiling (the GICD_TYPER CPUNumber field is 3 bits → max 8 CPU
# interfaces; the GICD_SGIR target list is 8 bits, 1 per CPU). Past 8 needs
# GICv3 (a separate lift). On REAL RPi3 hardware GICD_TYPER is unavailable (the
# BCM2837 is NOT a GIC) — there the count is a build-constant 4; this cert is
# QEMU-virt (where GICD_TYPER is exact).
#
# Then TWO load-bearing falsifiers:
#   (a) -DSMP_MUTEX_NOLOCK : the shared-counter increment BYPASSES the BKL. Under
#       -smp 8 with truly-concurrent tasks the RMW races -> lost updates ->
#       SMP-MUTEX: FAIL (total < expected). Proves the BKL is load-bearing.
#   (b) -DSMP_FORCE_NCPU=8 : ignore GICD_TYPER, HARDCODE the count to 8. Booted
#       under -smp 2 the bringup loop tries PSCI CPU_ON for cores 2..7 that DON'T
#       EXIST → the cert FAILs (SMP-RUN/MUTEX/BOOT: FAIL, total != 2*K). Proves
#       the GICD_TYPER detection is load-bearing — without it the same -smp 2
#       boot can't adapt.
#
# HONESTY (MC-2 §4.4, inherited): QEMU TCG models memory strongly and may MASK a
# missing-barrier/SMPEN race; the BKL's barrier teeth are only fully [live] on
# RPi3 hardware. A QEMU green proves "the lock serializes + N CPUs run distinct
# tasks + the detect reads the right count", NOT the barrier discipline on weak
# silicon.
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
# -nic none: -M virt otherwise auto-creates a virtio-net-pci that needs the
# efi-virtio.rom romfile (missing on minimal CI images → the advisory job's
# failure class). This test never touches the network, so suppress the NIC.
QEMU_BASE_FLAGS="-M virt -cpu cortex-a53 -m 256M \
                 -serial stdio -display none -no-reboot -nic none"

fail() { echo "[smp0] FAIL: $*" >&2; exit 1; }

run_qemu() {
    # $1 = log file, $2 = kernel image (a unique snapshot), $3 = -smp count.
    # The kernel boots into the T-Kernel shell and never exits on its own, so
    # the wall-clock timeout is the expected terminator; we only care about the
    # UART captured before it.
    log="$1"; img="$2"; nsmp="$3"
    cd "$BOOT"
    if command -v timeout >/dev/null 2>&1; then
        timeout -k 3 "$TIMEOUT_S" $QEMU $QEMU_BASE_FLAGS -smp "$nsmp" -kernel "$img" \
            >"$log" 2>&1 || true
    else
        $QEMU $QEMU_BASE_FLAGS -smp "$nsmp" -kernel "$img" >"$log" 2>&1 &
        qpid=$!
        sleep "$TIMEOUT_S"
        kill "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
}

# ── 1. The cert build (ONCE) — the SAME binary adapts to -smp 2/4/8 ──────
echo "=== [smp-autodetect]+[smp-2-tasks-run]+[smp-mutual-exclusion]+[smp-boot-survives] ==="
echo "=== ONE binary, three -smp runs (GICD_TYPER autodetect) ==="
cd "$BOOT"
make clean >/dev/null 2>&1
make EXTRA_CFLAGS=-DSMP_SELFTEST >/dev/null 2>&1 \
    || fail "build (SMP_SELFTEST) failed"
PASS_IMG="$(mktemp /tmp/smp0_pass.XXXXXX.elf)"
cp kernel.elf "$PASS_IMG"        # ONE snapshot, booted three times below

# K (per-task increments) is fixed at 200000 in main.c; the expected mutex
# total per run is N*K. Keep this in sync with main.c's K.
K=200000

for N in 2 4 8; do
    EXPECT=$((N * K))
    echo "----- -smp $N  (expect detected=$N, counter=$EXPECT) -----"
    LOG="$(mktemp)"
    run_qemu "$LOG" "$PASS_IMG" "$N"
    cat "$LOG"
    echo "..................................................................."

    grep -q "detected $N cpus via GICD_TYPER" "$LOG" \
        || fail "-smp $N: GICD_TYPER did not detect $N cpus"
    grep -q "shared counter=$EXPECT expected=$EXPECT" "$LOG" \
        || fail "-smp $N: shared counter != $EXPECT (autodetect/mutex wrong)"
    # The highest secondary for this N is cpu$((N-1)); assert it woke (proves
    # EXACTLY N-1 secondaries were released, not the hardcoded ceiling).
    grep -q "cpu$((N - 1)) entered dispatcher" "$LOG" \
        || fail "-smp $N: highest secondary cpu$((N-1)) never entered the dispatcher"
    grep -q "SMP-RUN: PASS" "$LOG" \
        || fail "-smp $N: no 'SMP-RUN: PASS' ($N CPUs did not run distinct tasks)"
    grep -q "SMP-MUTEX: PASS" "$LOG" \
        || fail "-smp $N: no 'SMP-MUTEX: PASS' (BKL did not serialize to $EXPECT)"
    grep -q "SMP-BOOT: PASS" "$LOG" \
        || fail "-smp $N: no 'SMP-BOOT: PASS' (a CPU wedged or the join timed out)"
    grep -q "Starting T-Kernel" "$LOG" \
        || fail "-smp $N: kernel did not reach the T-Kernel banner after the SMP slice"
    grep -q "Initial task started" "$LOG" \
        || fail "-smp $N: T-Kernel scheduler did not tick after the SMP slice"
    echo "[smp0] -smp $N: PASS (detected $N, counter $EXPECT, T-Kernel still boots)"
    rm -f "$LOG"
done
echo "[smp0] [smp-autodetect] PASS: ONE binary detected+adapted to -smp 2/4/8"
echo

# ── 2. Falsifier A: NOLOCK -> SMP-MUTEX: FAIL (BKL is load-bearing) ──────
echo "=== falsifier A: -DSMP_MUTEX_NOLOCK MUST produce SMP-MUTEX: FAIL (-smp 8) ==="
make clean >/dev/null 2>&1
make EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_MUTEX_NOLOCK" >/dev/null 2>&1 \
    || fail "build (SMP_MUTEX_NOLOCK) failed"
NOLOCK_IMG="$(mktemp /tmp/smp0_nolock.XXXXXX.elf)"
cp kernel.elf "$NOLOCK_IMG"

NOLOCK_LOG="$(mktemp)"
run_qemu "$NOLOCK_LOG" "$NOLOCK_IMG" 8
echo "----- captured UART (NOLOCK falsifier, -smp 8) -----"
cat "$NOLOCK_LOG"
echo "----------------------------------------------------"

grep -q "SMP-MUTEX: FAIL" "$NOLOCK_LOG" \
    || fail "NOLOCK falsifier did NOT lose updates (BKL not load-bearing?!)"
grep -q "Starting T-Kernel" "$NOLOCK_LOG" \
    || fail "NOLOCK falsifier wedged the primary (never reached T-Kernel)"
echo "[smp0] falsifier A: PASS (BKL bypass -> lost updates -> SMP-MUTEX: FAIL)"
echo

# ── 3. Falsifier B: FORCE_NCPU=8 under -smp 2 -> the cert FAILs ──────────
#    Hardcode the count to 8 (ignore GICD_TYPER); boot under -smp 2. The
#    bringup tries to wake cores 2..7 that don't exist → SMP-{RUN,MUTEX,BOOT}
#    FAIL and total != 2*K. Proves the GICD_TYPER detection is load-bearing.
echo "=== falsifier B: -DSMP_FORCE_NCPU=8 under -smp 2 MUST FAIL (detect is load-bearing) ==="
make clean >/dev/null 2>&1
make EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_FORCE_NCPU=8" >/dev/null 2>&1 \
    || fail "build (SMP_FORCE_NCPU=8) failed"
FORCE_IMG="$(mktemp /tmp/smp0_force.XXXXXX.elf)"
cp kernel.elf "$FORCE_IMG"

FORCE_LOG="$(mktemp)"
run_qemu "$FORCE_LOG" "$FORCE_IMG" 2
echo "----- captured UART (FORCE_NCPU=8 under -smp 2) -----"
cat "$FORCE_LOG"
echo "-----------------------------------------------------"

# The hardcoded build "detects" 8 and expects 1600000; under -smp 2 it can't
# wake cores 2..7 → at least one of SMP-RUN/MUTEX/BOOT must FAIL, and the
# counter must NOT reach 1600000 (=8*K).
grep -q "SMP-MUTEX: FAIL" "$FORCE_LOG" \
    || fail "FORCE_NCPU=8 under -smp 2 did NOT FAIL the mutex cert (detect not load-bearing?!)"
if grep -q "shared counter=1600000 expected=1600000" "$FORCE_LOG"; then
    fail "FORCE_NCPU=8 under -smp 2 somehow reached 8*K (absent cores ran?!)"
fi
echo "[smp0] falsifier B: PASS (hardcoded-8 can't adapt to -smp 2 -> cert FAILs)"
echo

rm -f "$PASS_IMG" "$NOLOCK_IMG" "$NOLOCK_LOG" "$FORCE_IMG" "$FORCE_LOG"
echo "=== ②.N8 full-SMP + [smp-autodetect] : ALL PASS ==="
