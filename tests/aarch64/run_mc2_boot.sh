#!/bin/sh
# tests/aarch64/run_mc2_boot.sh
#
# [mc2-boot-survives] cert harness (docs/architecture/
# mc2-baremetal-smp-plan.md §4.2/§4.3).
#
# Builds the aarch64 bare-metal kernel WITH the MC-2.0 SMP boot self-test
# (-DMC2_SMP_SELFTEST), boots it under QEMU virt with -smp 4 (so the
# parked secondary cores actually exist in the VM), captures the UART, and
# asserts:
#   1. MC2-BOOT: PASS                       (secondary woke + tile correct)
#   2. [BOOT] Starting T-Kernel...          (the primary still boots the
#                                            scheduler AFTER the wake)
#   3. [T-Kernel] Initial task started      (the scheduler actually ticks)
#
# It also runs the FAULTING-tile variant and asserts the primary SURVIVES
# (MC2-BOOT: FAIL join-timeout) and STILL boots the T-Kernel — i.e. a bad
# worker does not wedge the primary's join forever.
#
# Exit 0 = cert PASS; non-zero = FAIL. Greps the UART for the verdict
# lines; nothing here is paper.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
BOOT="$HERE/../../boot/aarch64"
QEMU="qemu-system-aarch64"
TIMEOUT_S="${MC2_TIMEOUT:-40}"

# NOTE (②.1b harness-snapshot fix): boot QEMU from a UNIQUE snapshot of
# kernel.elf (run_qemu $2), not the live build-dir image, so a concurrent
# rebuild (a parallel auditor's `make`, or this harness's own faulting-tile
# build) cannot corrupt an in-flight QEMU read.
QEMU_SMP_FLAGS="-M virt -cpu cortex-a53 -smp 4 -m 256M \
                -serial stdio -display none -no-reboot"

fail() { echo "[mc2-boot-survives] FAIL: $*" >&2; exit 1; }

run_qemu() {
    # $1 = log file, $2 = kernel image (a unique snapshot). Boots with a hard
    # wall-clock timeout; the kernel never exits on its own (it boots into the
    # T-Kernel idle loop), so the timeout is the expected terminator. We only
    # care about the captured UART up to that point.
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

echo "=== [mc2-boot-survives] : normal tile under -smp 4 ==="
cd "$BOOT"
make clean >/dev/null 2>&1
make EXTRA_CFLAGS=-DMC2_SMP_SELFTEST >/dev/null 2>&1 \
    || fail "build (MC2_SMP_SELFTEST) failed"
NORMAL_IMG="$(mktemp /tmp/mc2boot_normal.XXXXXX.elf)"
cp kernel.elf "$NORMAL_IMG"      # snapshot so a later rebuild can't corrupt it

NORMAL_LOG="$(mktemp)"
run_qemu "$NORMAL_LOG" "$NORMAL_IMG"
echo "----- captured UART (normal) -----"
cat "$NORMAL_LOG"
echo "----------------------------------"

grep -q "MC2-BOOT: PASS" "$NORMAL_LOG" \
    || fail "no 'MC2-BOOT: PASS' in UART (secondary did not wake / tile bad)"
grep -q "Starting T-Kernel" "$NORMAL_LOG" \
    || fail "kernel did not reach the T-Kernel banner after the wake"
grep -q "Initial task started" "$NORMAL_LOG" \
    || fail "T-Kernel scheduler did not tick (init task never ran)"
echo "[mc2-boot-survives] normal: PASS"
echo

echo "=== [mc2-boot-survives] : FAULTING tile (bounded join) under -smp 4 ==="
make clean >/dev/null 2>&1
make EXTRA_CFLAGS="-DMC2_SMP_SELFTEST -DMC2_FAULTING_TILE" >/dev/null 2>&1 \
    || fail "build (MC2_FAULTING_TILE) failed"
FAULT_IMG="$(mktemp /tmp/mc2boot_fault.XXXXXX.elf)"
cp kernel.elf "$FAULT_IMG"       # snapshot so a later rebuild can't corrupt it

FAULT_LOG="$(mktemp)"
run_qemu "$FAULT_LOG" "$FAULT_IMG"
echo "----- captured UART (faulting) -----"
cat "$FAULT_LOG"
echo "------------------------------------"

grep -q "MC2-BOOT: FAIL join-timeout" "$FAULT_LOG" \
    || fail "faulting tile did not produce the bounded-join timeout verdict"
grep -q "Starting T-Kernel" "$FAULT_LOG" \
    || fail "primary WEDGED on a faulting worker (never reached T-Kernel)"
grep -q "Initial task started" "$FAULT_LOG" \
    || fail "primary did not keep scheduling after a faulting worker"
echo "[mc2-boot-survives] faulting-join: PASS (primary survived a bad worker)"
echo

rm -f "$NORMAL_LOG" "$FAULT_LOG" "$NORMAL_IMG" "$FAULT_IMG"
echo "=== [mc2-boot-survives] : ALL PASS ==="