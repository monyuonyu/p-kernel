#!/bin/sh
# tests/aarch64/run_smp5.sh
#
# ②.2b-ii [smp-secondary-wait] cert harness
# (docs/architecture/smp-2b-ii-secondary-timer-plan.md).
#
# Builds the aarch64 bare-metal kernel WITH the ②.2b-ii self-test
# (-DSMP_SELFTEST -DSMP_SECONDARY_WAIT), boots it under QEMU virt with -smp 4,
# captures the UART, and asserts:
#
#   [smp-secondary-wait]  SMP-SECONDARY-WAIT: PASS — a REAL T-Kernel task on the
#       SECONDARY (CPU 1) can BLOCK and WAKE, in two halves:
#       (i)  SELF-TIMER WAKE: a task calls tk_dly_tsk and is woken by CPU 1's OWN
#            generic-timer tick (CPU 1 programs its own CNTP PPI 30).  NON-VACUITY:
#            the driver holds CPU 0's timer service OUT of the window (polls with
#            IRQ masked), so ONLY CPU 1's tick can expire the delay; knl_current_
#            time advancing >= the delay is the positive witness that CPU 1 ticked.
#       (ii) CROSS-CPU WAKE: a task blocks on tk_wai_sem(TMO_FEVR) — infinite, so
#            NO tick can wake it — and is woken ONLY by CPU 0's tk_sig_sem via the
#            cross-CPU wake (knl_make_ready → knl_smp_wake → SGI → the ②.2b-i async
#            hook re-dispatches it on CPU 1).  An SGI was delivered (sgi_taken>=1).
#       Run 10x: the PASS must be STABLE (no hang, no flake).
#
# Then builds each FALSIFIER and asserts it goes RED:
#   -DSMP_NO_SEC_TIMER  → CPU 1's CNTP unprogrammed → half (i)'s dly task hangs
#       (CPU 0's timer held out) → no SMP-SECONDARY-WAIT: PASS.  Proves the
#       secondary timer is load-bearing.
#   -DSMP_NO_XWAKE      → the cross-CPU IPI in knl_smp_wake is suppressed →
#       half (ii)'s sem-waiter never wakes → no SMP-SECONDARY-WAIT: PASS.  Proves
#       the cross-CPU IPI is load-bearing.
#
# ALSO asserts the [smp-uniproc-semantics] companion: the DEFAULT build (no
# -DSMP_SELFTEST) is .text BYTE-IDENTICAL to base (the SMP_BASE_SHA pin).  ②.2b-ii
# edits knl_make_ready (a shared-core function LINKED INTO THE DEFAULT BUILD) via
# an EMPTY preprocessor macro off-SMP, so the default .text MUST stay identical.
#
# HONESTY (inherited): QEMU TCG models memory strongly and may MASK a missing-
# barrier / SGI-or-tick-latency race.  A QEMU green proves the secondary timer +
# cross-CPU wake are correctly plumbed + load-bearing, NOT the barrier discipline
# on weak silicon (that is [live]-only on RPi3; RPi3 is also NOT GICv2 — its
# BCM2837 per-core timer/mailbox path is a deferred [live] follow-up).
#
# NOTE: the cert prints its verdict then the kernel boots to the interactive
# shell (an infinite sio_read_line loop), so QEMU does NOT exit — the harness
# uses a bounded timeout and greps the captured UART.  Exit 0 = cert PASS.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
BOOT="$HERE/../../boot/aarch64"
QEMU="qemu-system-aarch64"
TIMEOUT_S="${SMP5_TIMEOUT:-25}"

QEMU_SMP_FLAGS="-M virt -cpu cortex-a53 -smp 4 -m 256M -kernel kernel.elf \
                -serial stdio -display none -no-reboot"

fail() { echo "[smp5] FAIL: $*" >&2; exit 1; }

run_qemu() {
    log="$1"
    t="${2:-$TIMEOUT_S}"
    cd "$BOOT"
    if command -v timeout >/dev/null 2>&1; then
        timeout -k 3 "$t" $QEMU $QEMU_SMP_FLAGS >"$log" 2>&1 || true
    else
        $QEMU $QEMU_SMP_FLAGS >"$log" 2>&1 &
        qpid=$!
        sleep "$t"
        kill "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
}

# ── PASS build (run 10x; a flaky cert is a defect — ②.2c lesson) ─────────
echo "=== ②.2b-ii [smp-secondary-wait]: secondary CNTP tick + cross-CPU WAIT wake (-smp 4) ==="
cd "$BOOT"
make clean >/dev/null 2>&1
make EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_SECONDARY_WAIT" >/dev/null 2>&1 \
    || fail "build (SMP_SELFTEST + SMP_SECONDARY_WAIT) failed"

i=1
while [ "$i" -le 10 ]; do
    PASS_LOG="$(mktemp)"
    run_qemu "$PASS_LOG"
    if [ "$i" -le 1 ]; then
        echo "----- captured UART (②.2b-ii build, run $i) -----"
        cat "$PASS_LOG"
        echo "--------------------------------------------------"
    fi
    grep -aq "SMP-SECONDARY-WAIT: PASS" "$PASS_LOG" \
        || fail "run $i: no 'SMP-SECONDARY-WAIT: PASS' (secondary block/wake broke or hung)"
    grep -aq "Bdly: WOKE" "$PASS_LOG" \
        || fail "run $i: half (i) — Bdly was not woken by CPU 1's own tick"
    grep -aq "Bsem: WOKE" "$PASS_LOG" \
        || fail "run $i: half (ii) — Bsem was not woken by CPU 0's cross-CPU signal"
    grep -aq "p-kernel>" "$PASS_LOG" \
        || fail "run $i: T-Kernel did not boot to the shell after the cert (deadlock / BKL strand?)"
    echo "[smp5] run $i: SMP-SECONDARY-WAIT: PASS (both halves; boot survives)"
    rm -f "$PASS_LOG"
    i=$((i + 1))
done
echo "[smp5] PASS build: SMP-SECONDARY-WAIT: PASS STABLE across 10 runs"
echo "       => a secondary task blocks+wakes by its OWN tick (i) and by a CROSS-CPU signal (ii)"
echo

# ── FALSIFIER 1: -DSMP_NO_SEC_TIMER must go RED (run 5x) ─────────────────
echo "=== FALSIFIER -DSMP_NO_SEC_TIMER (must FAIL: CPU 1's CNTP unprogrammed → half (i) hangs) ==="
make clean >/dev/null 2>&1
make EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_SECONDARY_WAIT -DSMP_NO_SEC_TIMER" >/dev/null 2>&1 \
    || fail "falsifier build (+ SMP_NO_SEC_TIMER) failed"
i=1
while [ "$i" -le 5 ]; do
    F_LOG="$(mktemp)"
    run_qemu "$F_LOG"
    if grep -aq "SMP-SECONDARY-WAIT: PASS" "$F_LOG"; then
        echo "----- captured UART (SMP_NO_SEC_TIMER, run $i) -----"; cat "$F_LOG"
        fail "run $i: SMP_NO_SEC_TIMER PASSED — the secondary timer is NOT load-bearing (cert vacuous)!"
    fi
    grep -aq "Bdly: WOKE" "$F_LOG" \
        && fail "run $i: Bdly woke WITHOUT its own CNTP programmed — a non-CPU-1 tick wormed in (non-vacuity broken)"
    echo "[smp5] run $i: SMP_NO_SEC_TIMER → no PASS, half (i) hung (secondary timer load-bearing)"
    rm -f "$F_LOG"
    i=$((i + 1))
done
echo "[smp5] FALSIFIER SMP_NO_SEC_TIMER: RED across 5 runs (the secondary CNTP is LOAD-BEARING)"
echo

# ── FALSIFIER 2: -DSMP_NO_XWAKE must go RED (run 5x) ─────────────────────
echo "=== FALSIFIER -DSMP_NO_XWAKE (must FAIL: cross-CPU IPI suppressed → half (ii) never wakes) ==="
make clean >/dev/null 2>&1
make EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_SECONDARY_WAIT -DSMP_NO_XWAKE" >/dev/null 2>&1 \
    || fail "falsifier build (+ SMP_NO_XWAKE) failed"
i=1
while [ "$i" -le 5 ]; do
    F_LOG="$(mktemp)"
    run_qemu "$F_LOG"
    if grep -aq "SMP-SECONDARY-WAIT: PASS" "$F_LOG"; then
        echo "----- captured UART (SMP_NO_XWAKE, run $i) -----"; cat "$F_LOG"
        fail "run $i: SMP_NO_XWAKE PASSED — the cross-CPU IPI is NOT load-bearing (cert vacuous)!"
    fi
    grep -aq "Bsem: WOKE" "$F_LOG" \
        && fail "run $i: Bsem woke WITHOUT the cross-CPU IPI — the wake path is not what we think"
    echo "[smp5] run $i: SMP_NO_XWAKE → no PASS, half (ii) never woke (cross-CPU IPI load-bearing)"
    rm -f "$F_LOG"
    i=$((i + 1))
done
echo "[smp5] FALSIFIER SMP_NO_XWAKE: RED across 5 runs (the cross-CPU IPI is LOAD-BEARING)"
echo

# ── DEFAULT build .text byte-identity (the CROWN gate) ───────────────────
echo "=== [smp-uniproc-semantics]: DEFAULT build .text byte-identity (the crown) ==="
make clean >/dev/null 2>&1
make >/dev/null 2>&1 || fail "default build failed"
"${OBJCOPY:-aarch64-linux-gnu-objcopy}" -O binary -j .text kernel.elf /tmp/smp5_def_text.bin 2>/dev/null \
    || fail "objcopy .text failed"
DEF_SHA="$(sha256sum /tmp/smp5_def_text.bin | cut -d' ' -f1)"
echo "[smp5] DEFAULT build .text sha256 = $DEF_SHA"
SMP_BASE_SHA="755a20fae2d9b7415045193ea8287623dbeb906963609a63dce8a19c8a130513"
if [ "$DEF_SHA" != "$SMP_BASE_SHA" ]; then
    fail "DEFAULT .text sha $DEF_SHA != base $SMP_BASE_SHA — ②.2b-ii leaked into the shipped kernel!"
fi
echo "SMP-UNIPROC-SEMANTICS: PASS (.text byte-identical to base sha $SMP_BASE_SHA)"
if [ -n "${SMP_BASE_REF:-}" ] && [ -f "${SMP_BASE_REF}" ]; then
    cmp -s "${SMP_BASE_REF}" /tmp/smp5_def_text.bin \
        || fail "DEFAULT .text DIFFERS from base binary ${SMP_BASE_REF}"
    echo "SMP-UNIPROC-SEMANTICS: also byte-identical to base binary ${SMP_BASE_REF}"
fi
echo

echo "=== ②.2b-ii [smp-secondary-wait] + both falsifiers + [smp-uniproc-semantics] : ALL PASS ==="
echo "    A secondary task blocks+wakes by its OWN CNTP tick (i) and by a CROSS-CPU signal (ii);"
echo "    the crown .text is byte-identical.  QEMU green != hardware green (barrier/coherency on"
echo "    weak silicon + RPi3 BCM2837 timer/mailbox are [live]-only)."
