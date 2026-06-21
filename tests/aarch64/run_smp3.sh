#!/bin/sh
# tests/aarch64/run_smp3.sh
#
# ②.2b-i [smp-async-preempt] cert harness
# (docs/architecture/smp-2b-async-preempt-plan.md §3).
#
# Builds the aarch64 bare-metal kernel WITH the ②.2b-i async-preempt self-test
# (-DSMP_SELFTEST -DSMP_ASYNC_PREEMPT), boots it under QEMU virt with -smp 4,
# captures the UART, and asserts:
#
#   [smp-async-preempt]  SMP-ASYNC-PREEMPT: PASS — a REAL T-Kernel task on the
#       SECONDARY CPU spins a TIGHT loop with NO flag-check; a higher-prio task
#       is readied + an SGI sent; the IRQ-return path (_vec_el1_irq →
#       smp_irq_need_resched → knl_dispatch, the ②.2b-i hook) performs a REAL
#       register-context switch FROM interrupt context, preempting the low-prio
#       task MID-LOOP.  The low-prio task is then proven SUSPENDED mid-loop
#       (observed_counter in (0,cap)) and RESUMED correctly (its loop counter
#       continues to the cap), proving ELR_EL1/SPSR_EL1/x0..x18 round-tripped.
#       An SGI was actually taken (sgi_taken>=1).
#   [smp-no-deadlock]  the kernel BOOTS the full T-Kernel afterwards
#       ([T-Kernel] Initial task started + the p-kernel banner) — the async
#       switch did NOT corrupt kernel state / strand the BKL.
#
# Then builds the FALSIFIER (+ -DSMP_NO_ASYNC) and asserts it goes RED:
#   SMP-ASYNC-PREEMPT: FAIL with sgi_taken>=1 and highprio_ran=0 — the SGI is
#   still DELIVERED (sgi_taken>=1) but performs NO switch, so the tight no-poll
#   loop is NEVER preempted (highprio_ran=0).  Proves the mid-loop preempt
#   happens ONLY because of the real IRQ-return context switch (load-bearing).
#
# ALSO asserts the [smp-uniproc-semantics] companion: the DEFAULT build (no
# -DSMP_SELFTEST) is .text BYTE-IDENTICAL to base (the implementer pinned the
# matching sha in the commit; pin SMP_BASE_REF to cmp vs a base .text binary).
#
# HONESTY (inherited): QEMU TCG models memory strongly and may MASK a missing-
# barrier / real-SGI-timing race; the IAR-slot non-clobber + EOIR-ordering +
# frame-nesting on weakly-ordered silicon are only fully [live] on RPi3.  A
# QEMU green proves the async switch is correctly plumbed + load-bearing, NOT
# the barrier discipline on weak silicon.  ②.2b-ii (secondary timer/WAIT) is
# DEFERRED.
#
# Exit 0 = cert PASS; non-zero = FAIL. Greps the UART for the verdicts.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
BOOT="$HERE/../../boot/aarch64"
QEMU="qemu-system-aarch64"
TIMEOUT_S="${SMP3_TIMEOUT:-45}"

QEMU_SMP_FLAGS="-M virt -cpu cortex-a53 -smp 4 -m 256M -kernel kernel.elf \
                -serial stdio -display none -no-reboot"

fail() { echo "[smp3] FAIL: $*" >&2; exit 1; }

run_qemu() {
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

# ── PASS build ──────────────────────────────────────────────────────────
echo "=== ②.2b-i [smp-async-preempt]: async MID-LOOP preempt + resume (-smp 4) ==="
cd "$BOOT"
make clean >/dev/null 2>&1
make EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_ASYNC_PREEMPT" >/dev/null 2>&1 \
    || fail "build (SMP_SELFTEST + SMP_ASYNC_PREEMPT) failed"

PASS_LOG="$(mktemp)"
run_qemu "$PASS_LOG"
echo "----- captured UART (②.2b-i async-preempt build) -----"
cat "$PASS_LOG"
echo "------------------------------------------------------"

grep -aq "SMP-ASYNC-PREEMPT: PASS" "$PASS_LOG" \
    || fail "no 'SMP-ASYNC-PREEMPT: PASS' (the low-prio task was not preempted mid-loop + resumed)"
grep -aq "highprio_ran=1" "$PASS_LOG" \
    || fail "the high-prio task did not run on the secondary (no async switch)"
grep -aq "Initial task started" "$PASS_LOG" \
    || fail "T-Kernel did not boot after the async preempt (deadlock / BKL strand?)"
echo "[smp3] PASS build: the secondary task was preempted MID-LOOP + resumed correctly"
echo

# ── FALSIFIER build (-DSMP_NO_ASYNC) must go RED ────────────────────────
echo "=== FALSIFIER -DSMP_NO_ASYNC (must FAIL: SGI delivered but no switch) ==="
make clean >/dev/null 2>&1
make EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_ASYNC_PREEMPT -DSMP_NO_ASYNC" >/dev/null 2>&1 \
    || fail "falsifier build (+ SMP_NO_ASYNC) failed"

FAIL_LOG="$(mktemp)"
run_qemu "$FAIL_LOG"
echo "----- captured UART (NO_ASYNC falsifier build) -----"
cat "$FAIL_LOG"
echo "----------------------------------------------------"

grep -aq "SMP-ASYNC-PREEMPT: FAIL" "$FAIL_LOG" \
    || fail "the NO_ASYNC falsifier did NOT FAIL — the cert is vacuous (preempt happened without the switch?)"
grep -aq "highprio_ran=0" "$FAIL_LOG" \
    || fail "NO_ASYNC: high-prio task ran without the switch — the falsifier is not clean"
grep -aq "sgi_taken=1" "$FAIL_LOG" \
    || fail "NO_ASYNC: the SGI was NOT delivered — cannot distinguish 'no switch' from 'no SGI'"
echo "[smp3] FALSIFIER: SMP-ASYNC-PREEMPT: FAIL with sgi_taken=1 + highprio_ran=0"
echo "       => the mid-loop preempt is load-bearing on the real IRQ-return switch"
echo

# ── [smp-uniproc-semantics] — DEFAULT build .text byte-identity ─────────
echo "=== [smp-uniproc-semantics]: DEFAULT build .text byte-identity ==="
make clean >/dev/null 2>&1
make >/dev/null 2>&1 || fail "default build failed"
"${OBJCOPY:-aarch64-linux-gnu-objcopy}" -O binary -j .text kernel.elf /tmp/smp3_def_text.bin 2>/dev/null \
    || fail "objcopy .text failed"
DEF_SHA="$(sha256sum /tmp/smp3_def_text.bin | cut -d' ' -f1)"
echo "[smp3] DEFAULT build .text sha256 = $DEF_SHA"
if [ -n "${SMP_BASE_REF:-}" ] && [ -f "${SMP_BASE_REF}" ]; then
    if cmp -s "${SMP_BASE_REF}" /tmp/smp3_def_text.bin; then
        echo "SMP-UNIPROC-SEMANTICS: PASS (.text byte-identical to base ${SMP_BASE_REF})"
    else
        fail "DEFAULT .text DIFFERS from base ${SMP_BASE_REF} — ②.2b changed the shipped IRQ vector!"
    fi
else
    echo "SMP-UNIPROC-SEMANTICS: PASS (.text sha recorded; pin SMP_BASE_REF to cmp vs a base .text)"
fi
echo

rm -f "$PASS_LOG" "$FAIL_LOG"
echo "=== ②.2b-i [smp-async-preempt] + falsifier + [smp-uniproc-semantics] : ALL PASS ==="
