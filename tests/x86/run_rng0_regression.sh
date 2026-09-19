#!/bin/sh
# tests/x86/run_rng0_regression.sh
#
# RNG0-BUSY-TASK-STALLS-DISPATCH matched-arm regression probe
# (gap-ledger.md: RNG0-BUSY-TASK-STALLS-DISPATCH, judgment-pending item 8).
#
# WHY THIS EXISTS
# ----------------
# A ring0 task that never makes a single kernel call (a bare busy-spin, no
# syscalls at all) blocks the timer tick's own dispatch decision from ever
# being enacted, starving every OTHER task regardless of priority — see
# gap-ledger.md for the traced root cause (END_CRITICAL_SECTION's
# !knl_isTaskIndependent() guard is unconditionally true for the whole body
# of knl_timer_handler, so knl_dispatch() is never called from the tick).
# This script builds and boots the T16 probe (arch/x86/selftest.c, guarded
# behind -DPK_RNG0_REGR_TEST so the default build's .text is untouched) in
# both of its two arms and reports which one is RED and which is GREEN.
#
# THE TWO ARMS
# ------------
#   positive (-DPK_RNG0_REGR_TEST only): a busy ring0 task that never yields.
#     Expected on today's unfixed master: the init task's tk_dly_tsk(500)
#     NEVER RETURNS -> the boot hangs until this script's timeout -> RED.
#   control (also -DPK_RNG0_REGR_CONTROL): the same busy task, but it calls
#     tk_dly_tsk(10) every iteration (voluntarily re-enters the kernel).
#     Expected: tk_dly_tsk(500) returns normally within ~550ms -> GREEN.
# A control arm that does not go GREEN means the PROBE is broken, not the
# kernel — do not trust a positive-arm RED without a green control arm run
# in the same session.
#
# THIS DOES NOT FIX THE BUG. It does not decide between gap-ledger's
# options (a)/(b)/(c) for RNG0-BUSY-TASK-STALLS-DISPATCH — that is a human
# design decision (judgment-pending item 8). It is also NOT wired into CI
# yet; that is a separate decision for later.
#
# USAGE
#   ARM=positive sh tests/x86/run_rng0_regression.sh
#   ARM=control  sh tests/x86/run_rng0_regression.sh
#
# ENVIRONMENT
#   ARM                     positive | control              (required)
#   RNG0_BOOT_TIMEOUT       wall-clock cap per boot, s       (default 20)
#   RNG0_SKIP_BUILD         1 = reuse the existing build     (default 0)
#   RNG0_KEEP               1 = keep the scratch dir + logs  (default 0)
#   QEMU                    override qemu binary
#
# EXIT CODE
#   0  arm behaved as expected for today's known-bug baseline
#      (positive -> RED, control -> GREEN)
#   1  control arm did NOT go GREEN — the probe itself is broken, or the
#      build/boot failed outright. Do not trust any positive-arm result
#      from the same session until this is fixed.
#   2  positive arm unexpectedly went GREEN — i.e. tk_dly_tsk(500) returned
#      even with a non-yielding busy task running. This would mean the bug
#      no longer reproduces on this tree; it is NEWS, not a script bug —
#      update gap-ledger.md's RNG0-BUSY-TASK-STALLS-DISPATCH row rather than
#      silently trusting green.
#
# The machine-readable verdict line, always printed, always greppable:
#   [rng0-regr] arm=<positive|control> verdict=<RED|GREEN> boots=N/N
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BOOT="$ROOT/boot/x86"

ARM="${ARM:-}"
case "$ARM" in
    positive|control) ;;
    *) echo "[rng0-regr] FATAL: set ARM=positive or ARM=control" >&2; exit 1 ;;
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

if [ "$ARM" = "positive" ]; then
    DEFS="-DPK_RNG0_REGR_TEST"
    EXPECT="RED"
else
    DEFS="-DPK_RNG0_REGR_TEST -DPK_RNG0_REGR_CONTROL"
    EXPECT="GREEN"
fi

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
    echo "[rng0-regr] PASS: arm=$ARM matches today's known-bug baseline ($EXPECT)."
    exit 0
fi

if [ "$ARM" = "control" ]; then
    echo "[rng0-regr] FAIL: control arm went RED — the PROBE is broken, not proven about the kernel." >&2
    echo "[rng0-regr]       Do not trust any positive-arm RED from this session until this is fixed." >&2
    exit 1
else
    echo "[rng0-regr] NEWS: positive arm went GREEN — tk_dly_tsk(500) returned despite a" >&2
    echo "[rng0-regr]       non-yielding busy task. RNG0-BUSY-TASK-STALLS-DISPATCH may no longer" >&2
    echo "[rng0-regr]       reproduce on this tree. Update gap-ledger.md, do not just re-run." >&2
    exit 2
fi
