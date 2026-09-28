#!/bin/sh
# tests/x86/run_rng0_regression.sh
#
# RNG0-BUSY-TASK-STALLS-DISPATCH matched-arm regression test
# (gap-ledger.md: RNG0-BUSY-TASK-STALLS-DISPATCH; ROADMAP 0-1).
#
# WHY THIS EXISTS
# ----------------
# A ring0 task that never makes a single kernel call (a bare busy-spin, no
# syscalls at all) used to block the timer tick's own dispatch decision from
# ever being enacted, starving every OTHER task regardless of priority — see
# gap-ledger.md for the traced root cause (END_CRITICAL_SECTION's
# !knl_isTaskIndependent() guard is unconditionally true for the whole body
# of knl_timer_handler, so knl_dispatch() was never called from the tick).
# The fix (2026-09-28) is a delayed dispatch at the exit of the outermost
# IRQ (knl_irq_exit_dispatch, cpu_cntl.c, called at the end of irq_handler in
# boot/x86/idt.c): the same point every other μT-Kernel port dispatches from.
# This script builds and boots the T16 probe (arch/x86/selftest.c, guarded
# behind -DPK_RNG0_REGR_TEST so the default build's .text is untouched).
#
# THE THREE ARMS
# --------------
#   positive (-DPK_RNG0_REGR_TEST): a busy ring0 task (pri 20) that never
#     yields; init (pri 1) waits on tk_dly_tsk(500). Expected: returns -> GREEN.
#   control (also -DPK_RNG0_REGR_CONTROL): the busy task calls tk_dly_tsk(10)
#     every iteration. Expected: GREEN. (A red control means the PROBE is
#     broken, not the kernel.)
#   nofix (also -DPK_RNG0_NO_EXIT_DISPATCH): the positive arm on a kernel
#     with the IRQ-exit dispatch compiled out — the pre-2026-09-28 behaviour.
#     Expected: tk_dly_tsk(500) never returns -> RED. This is the negative
#     control that shows the positive arm's green comes from the fix.
#
# USAGE
#   ARM=positive sh tests/x86/run_rng0_regression.sh
#   ARM=control  sh tests/x86/run_rng0_regression.sh
#   ARM=nofix    sh tests/x86/run_rng0_regression.sh
#
# ENVIRONMENT
#   ARM                     positive | control | nofix       (required)
#   RNG0_BOOT_TIMEOUT       wall-clock cap per boot, s       (default 20)
#   RNG0_SKIP_BUILD         1 = reuse the existing build     (default 0)
#   RNG0_KEEP               1 = keep the scratch dir + logs  (default 0)
#   QEMU                    override qemu binary
#
# EXIT CODE
#   0  the arm behaved as expected (positive/control GREEN, nofix RED)
#   1  it did not, or the build/boot failed outright
#
# The machine-readable verdict line, always printed, always greppable:
#   [rng0-regr] arm=<positive|control|nofix> verdict=<RED|GREEN> boots=N/N
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BOOT="$ROOT/boot/x86"

ARM="${ARM:-}"
case "$ARM" in
    positive|control|nofix) ;;
    *) echo "[rng0-regr] FATAL: set ARM=positive, ARM=control or ARM=nofix" >&2; exit 1 ;;
esac

BOOT_TIMEOUT="${RNG0_BOOT_TIMEOUT:-20}"
SKIP_BUILD="${RNG0_SKIP_BUILD:-0}"
KEEP="${RNG0_KEEP:-0}"
QEMU="${QEMU:-qemu-system-x86_64}"

command -v "$QEMU" >/dev/null 2>&1 || { echo "[rng0-regr] FATAL: $QEMU not found" >&2; exit 1; }

SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/rng0regr.XXXXXX")"
cleanup() {
    if [ -n "${QPID:-}" ]; then kill "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true; fi
    if [ "$KEEP" != "1" ]; then rm -rf "$SCRATCH"; fi
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM

case "$ARM" in
    positive) DEFS="-DPK_RNG0_REGR_TEST";                            EXPECT="GREEN" ;;
    control)  DEFS="-DPK_RNG0_REGR_TEST -DPK_RNG0_REGR_CONTROL";     EXPECT="GREEN" ;;
    nofix)    DEFS="-DPK_RNG0_REGR_TEST -DPK_RNG0_NO_EXIT_DISPATCH"; EXPECT="RED" ;;
esac

if [ "$SKIP_BUILD" = "1" ]; then
    echo "[rng0-regr] RNG0_SKIP_BUILD=1 — reusing the existing $BOOT build"
else
    echo "[rng0-regr] clean build: $BOOT arm=$ARM ($DEFS)"
    ( cd "$BOOT" && make clean ) >"$SCRATCH/build.log" 2>&1 || true
    if ! ( cd "$BOOT" && make EXTRA_CFLAGS="$DEFS" bootloader.bin ) >>"$SCRATCH/build.log" 2>&1; then
        tail -60 "$SCRATCH/build.log" >&2
        echo "[rng0-regr] FATAL: build failed (full log: $SCRATCH/build.log)" >&2
        exit 1
    fi
fi
[ -f "$BOOT/bootloader.bin" ] || { echo "[rng0-regr] FATAL: $BOOT/bootloader.bin missing after build" >&2; exit 1; }

LOG="$SCRATCH/serial.log"
: > "$LOG"
echo "[rng0-regr] booting arm=$ARM, cap ${BOOT_TIMEOUT}s"
( cd "$BOOT" && timeout "$BOOT_TIMEOUT" "$QEMU" -m 256 -kernel bootloader.bin \
    -serial file:"$LOG" -cpu qemu64 -display none -no-reboot ) >"$SCRATCH/qemu.log" 2>&1 &
QPID=$!
wait "$QPID" 2>/dev/null || true
QPID=""

if grep -aqF '[T16] PASS' "$LOG"; then
    VERDICT="GREEN"
else
    VERDICT="RED"
fi

echo "[rng0-regr] serial tail:"
tail -5 "$LOG" | sed 's/^/[rng0-regr]   /'
echo "[rng0-regr] arm=$ARM verdict=$VERDICT boots=1/1"

if [ "$KEEP" = "1" ]; then
    echo "[rng0-regr] RNG0_KEEP=1 — serial log left at $LOG"
fi

if [ "$VERDICT" = "$EXPECT" ]; then
    echo "[rng0-regr] PASS: arm=$ARM is $EXPECT as expected."
    exit 0
fi

case "$ARM" in
    control)
        echo "[rng0-regr] FAIL: control arm went RED — the PROBE is broken, not proven about the kernel." >&2 ;;
    positive)
        echo "[rng0-regr] FAIL: a non-yielding busy ring0 task starved a higher-priority sleeper" >&2
        echo "[rng0-regr]       (tk_dly_tsk(500) never returned). The IRQ-exit dispatch is missing or broken." >&2 ;;
    nofix)
        echo "[rng0-regr] FAIL: with the IRQ-exit dispatch compiled out the probe still went GREEN," >&2
        echo "[rng0-regr]       so the positive arm's green is not shown to come from the fix." >&2 ;;
esac
exit 1
