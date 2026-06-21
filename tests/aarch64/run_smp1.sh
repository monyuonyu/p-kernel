#!/bin/sh
# tests/aarch64/run_smp1.sh
#
# ②.1a cross-CPU preemption cert harness
# (docs/architecture/smp-1-ipi-preempt-plan.md §1/§4).
#
# Builds the aarch64 bare-metal kernel WITH the ②.1a preempt self-test
# (-DSMP_SELFTEST -DSMP_PREEMPT_TEST), boots it under QEMU virt -smp 4
# (uses the boot CPU + ONE secondary), captures the UART, and asserts:
#
#   [smp-cross-preempt]  SMP-PREEMPT: PASS — CPU B, spinning on a LOW-prio
#                        task, is forced to switch to a HIGH-prio task that
#                        CPU A readied and delivered via smp_send_reschedule(B)
#                        (a GIC SGI / IPI), within a bounded watchdog. Proven
#                        by: cpu1 preempted_at != 0, ran_high-prio=1,
#                        sgi_taken>=1, and B's ctxtsk == the high-prio task.
#                        Plus [smp-boot-survives]: T-Kernel still boots
#                        afterwards ([BOOT] Starting T-Kernel.../Initial task).
#
# Then the LOAD-BEARING falsifier: rebuild with -DSMP_NO_IPI so
# smp_send_reschedule() is a no-op. Everything else is identical (the
# high-prio task is still readied, B is still spinning with IRQs unmasked,
# the SGI handler is still registered) — but NO SGI is delivered, so B never
# preempts. The bounded watchdog reports the miss -> SMP-PREEMPT: FAIL. This
# proves the IPI is load-bearing (the preemption happens because of, and ONLY
# because of, the SGI). The exact analogue of ②.0's -DSMP_MUTEX_NOLOCK.
#
# HONESTY: the SGI is QEMU-virt-GICv2-specific (GICD_SGIR). RPi3 uses the
# BCM2837 mailbox, NOT GICD_SGIR — the RPi3 [live] port is a deferred
# follow-up. QEMU TCG models memory strongly and may MASK the dsb-ish
# ordering / deliver the SGI with more forgiving timing than silicon; the
# real barrier/latency teeth are [live] only on RPi3 hardware. A QEMU green
# proves "the SGI path is correctly plumbed and load-bearing", NOT the
# barrier discipline on weakly-ordered silicon.
#
# Exit 0 = cert PASS; non-zero = FAIL. Greps the UART for the verdicts.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
BOOT="$HERE/../../boot/aarch64"
QEMU="qemu-system-aarch64"
TIMEOUT_S="${SMP1_TIMEOUT:-45}"

# Snapshot the built ELF to a unique path before booting so a later rebuild
# (the falsifier) can't corrupt an in-flight QEMU read (②.1b harness-fragility
# fix, applied proactively here).
QEMU_BASE_FLAGS="-M virt -cpu cortex-a53 -smp 4 -m 256M \
                 -serial stdio -display none -no-reboot"

fail() { echo "[smp1] FAIL: $*" >&2; exit 1; }

run_qemu() {
    # $1 = log file, $2 = kernel image. The kernel boots into the T-Kernel
    # shell and never exits on its own, so the wall-clock timeout is the
    # expected terminator; we only care about the UART captured before it.
    log="$1"; img="$2"
    cd "$BOOT"
    if command -v timeout >/dev/null 2>&1; then
        timeout -k 3 "$TIMEOUT_S" $QEMU $QEMU_BASE_FLAGS -kernel "$img" \
            >"$log" 2>&1 || true
    else
        $QEMU $QEMU_BASE_FLAGS -kernel "$img" >"$log" 2>&1 &
        qpid=$!
        sleep "$TIMEOUT_S"
        kill "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
}

# ── 1. The cert build: SMP-PREEMPT: PASS ────────────────────────────────
echo "=== ②.1a [smp-cross-preempt] under -smp 4 (GIC SGI IPI) ==="
cd "$BOOT"
make clean >/dev/null 2>&1
make EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_PREEMPT_TEST" >/dev/null 2>&1 \
    || fail "build (SMP_PREEMPT_TEST) failed"
PASS_IMG="$(mktemp /tmp/smp1_pass.XXXXXX.elf)"
cp kernel.elf "$PASS_IMG"

PASS_LOG="$(mktemp)"
run_qemu "$PASS_LOG" "$PASS_IMG"
echo "----- captured UART (preempt cert build) -----"
cat "$PASS_LOG"
echo "----------------------------------------------"

grep -aq "cpu1 entered dispatcher (preempt cert)" "$PASS_LOG" \
    || fail "secondary CPU never entered the preempt dispatcher"
grep -aq "SMP-PREEMPT: PASS" "$PASS_LOG" \
    || fail "no 'SMP-PREEMPT: PASS' (B did not preempt to the high-prio task)"
grep -aq "Starting T-Kernel" "$PASS_LOG" \
    || fail "kernel did not reach the T-Kernel banner after the preempt slice"
grep -aq "Initial task started" "$PASS_LOG" \
    || fail "T-Kernel scheduler did not tick after the preempt slice"
echo "[smp1] cert build: PASS (SMP-PREEMPT, T-Kernel still boots)"
echo

# ── 2. The load-bearing falsifier: NO_IPI -> SMP-PREEMPT: FAIL ──────────
echo "=== ②.1a falsifier: -DSMP_NO_IPI MUST produce SMP-PREEMPT: FAIL ==="
make clean >/dev/null 2>&1
make EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_PREEMPT_TEST -DSMP_NO_IPI" \
    >/dev/null 2>&1 || fail "build (SMP_NO_IPI) failed"
NOIPI_IMG="$(mktemp /tmp/smp1_noipi.XXXXXX.elf)"
cp kernel.elf "$NOIPI_IMG"

NOIPI_LOG="$(mktemp)"
run_qemu "$NOIPI_LOG" "$NOIPI_IMG"
echo "----- captured UART (NO_IPI falsifier) -----"
cat "$NOIPI_LOG"
echo "--------------------------------------------"

grep -aq "SMP-PREEMPT: FAIL" "$NOIPI_LOG" \
    || fail "NO_IPI falsifier did NOT miss the preempt (SGI not load-bearing?!)"
# The missed preempt must not crash the primary — it only fails to preempt.
grep -aq "Starting T-Kernel" "$NOIPI_LOG" \
    || fail "NO_IPI falsifier wedged the primary (never reached T-Kernel)"
echo "[smp1] falsifier: PASS (no SGI -> no preempt -> SMP-PREEMPT: FAIL, as required)"
echo

rm -f "$PASS_LOG" "$NOIPI_LOG" "$PASS_IMG" "$NOIPI_IMG"
echo "=== ②.1a cross-CPU preempt : ALL PASS ==="
