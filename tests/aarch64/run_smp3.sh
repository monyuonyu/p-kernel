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
#   [smp-no-deadlock]  the §5.4 BKL-held reschedule guard is CERTIFIED.  A REAL
#       task L on the SECONDARY ACQUIRES the BKL (enters a kernel critical
#       section) and spins; an SGI for a higher-prio task H lands WHILE L holds
#       the BKL.  The guard (smp_irq_need_resched: g_bkl_owner==me → return 0)
#       DEFERS the async switch → L's critical section completes ATOMICALLY
#       (obs_released=1), L releases the BKL, the DEFERRED reschedule fires AFTER
#       release → H runs + acquires the BKL CLEANLY (hi_got_bkl=1) → no deadlock →
#       SMP-NO-DEADLOCK: PASS.  FALSIFIER -DSMP_NO_BKL_GUARD removes the guard →
#       the switch fires mid-critical-section → H wedges forever waiting for the
#       BKL L stranded → PERMANENT DEADLOCK (no PASS; the harness timeout is the
#       watchdog).  Proves the guard is LOAD-BEARING.
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
    t="${2:-$TIMEOUT_S}"          # optional per-run timeout (deadlock runs use a short one)
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
grep -aq "p-kernel>" "$PASS_LOG" \
    || fail "T-Kernel did not boot after the async preempt (deadlock / BKL strand?)"
echo "[smp3] PASS build: the secondary task was preempted MID-LOOP + resumed correctly"
echo

# ── [smp-no-deadlock] PASS build (the §5.4 BKL-held guard, CERTIFIED) ────
# A REAL task L on the SECONDARY ACQUIRES the BKL (enters a kernel critical
# section) and spins; an SGI for a higher-prio task H lands WHILE L holds the
# BKL.  The §5.4 guard (smp.c smp_irq_need_resched: g_bkl_owner==me → return 0)
# DEFERS the async switch — L's critical section completes ATOMICALLY, L releases
# the BKL, the DEFERRED reschedule (the pending flag was KEPT, not lost) re-fires
# AFTER release → H runs + acquires the BKL CLEANLY → no deadlock.
echo "=== ②.2b [smp-no-deadlock]: BKL-held mid-crit preempt is DEFERRED (guard CERTIFIED) ==="
make clean >/dev/null 2>&1
make EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_DEADLOCK_TEST" >/dev/null 2>&1 \
    || fail "build (SMP_SELFTEST + SMP_DEADLOCK_TEST) failed"
DL_LOG="$(mktemp)"
run_qemu "$DL_LOG"
echo "----- captured UART (②.2b no-deadlock guard'd build) -----"
cat "$DL_LOG"
echo "----------------------------------------------------------"
grep -aq "SMP-NO-DEADLOCK: PASS" "$DL_LOG" \
    || fail "no 'SMP-NO-DEADLOCK: PASS' (the BKL-held guard did not defer / deadlocked / lost the reschedule)"
grep -aq "obs_released=1" "$DL_LOG" \
    || fail "the critical section was NOT atomic (H ran mid-critical-section — guard did not defer)"
grep -aq "hi_got_bkl=1" "$DL_LOG" \
    || fail "the high-prio task could NOT acquire the BKL (lock stranded)"
echo "[smp3] [smp-no-deadlock] PASS: BKL-held switch DEFERRED, deferred reschedule fired, no deadlock"
echo

# ── [smp-no-deadlock] FALSIFIER (-DSMP_NO_BKL_GUARD) must DEADLOCK ───────
# Remove the §5.4 guard clause → the async switch fires WHILE L holds the BKL →
# the switched-to task H is suspended-L's deadlock victim (it waits forever for L
# to release the BKL, but L is suspended mid-critical-section holding it) →
# PERMANENT DEADLOCK.  The harness's bounded timeout IS the watchdog: with the
# guard gone, the cert NEVER prints PASS and the deadlock evidence is present:
#   "H: high-prio ran; waiting for L to have RELEASED the BKL"  AND
#   NO "H: L released cleanly"  AND  NO "SMP-NO-DEADLOCK: PASS".
# This proves the guard is LOAD-BEARING: WITH → PASS, WITHOUT → deadlock.
echo "=== FALSIFIER -DSMP_NO_BKL_GUARD (must DEADLOCK: switch fires mid-crit, no guard) ==="
make clean >/dev/null 2>&1
make EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_DEADLOCK_TEST -DSMP_NO_BKL_GUARD" >/dev/null 2>&1 \
    || fail "falsifier build (+ SMP_NO_BKL_GUARD) failed"
# The deadlock wedges the boot: smp_dl_test_run() never returns, so usermain
# never reaches the post-cert "[ai] ... AI primitives" init OR the shell prompt.
# A bounded timeout is the watchdog: the run NEVER prints PASS and NEVER makes
# post-cert progress.  (25s gives the SGI handshake time to land H mid-crit.)
DLF_LOG="$(mktemp)"
run_qemu "$DLF_LOG" "${SMP3_DEADLOCK_TIMEOUT:-25}"
echo "----- captured UART (NO_BKL_GUARD falsifier build) -----"
cat "$DLF_LOG"
echo "--------------------------------------------------------"
# (1) The falsifier MUST NOT reach PASS (the guard that prevents the deadlock is
#     gone → the async switch fires mid-critical-section → deadlock).
if grep -aq "SMP-NO-DEADLOCK: PASS" "$DLF_LOG"; then
    fail "the NO_BKL_GUARD falsifier PASSED — the guard is NOT load-bearing (cert vacuous)!"
fi
# (2) It MUST NOT cleanly complete the round-trip (H acquiring the BKL cleanly
#     would mean no strand → no deadlock).
if grep -aq "H: L released cleanly" "$DLF_LOG"; then
    fail "H acquired the BKL cleanly WITHOUT the guard — the deadlock did not reproduce (flaky?)"
fi
# (3) The boot MUST be WEDGED: smp_dl_test_run() never returned, so usermain
#     never reached the post-cert progress marker.  (In the PASS build that line
#     always appears; its ABSENCE here is the deadlock — robust to UART garble
#     and to exactly where the wedge lands, L-mid-crit or H-waiting.)
if grep -aq "p-kernel>" "$DLF_LOG"; then
    fail "the boot reached the shell prompt — smp_dl_test_run() returned → NO deadlock (guard not load-bearing?)"
fi
# (4) Positive evidence the deadlock scenario was actually entered: L provably
#     ACQUIRED the BKL (entered the critical section) — so the wedge is the
#     BKL-held-switch deadlock, not an unrelated early failure.
grep -aq "L: BKL acquired; spinning in critical section" "$DLF_LOG" \
    || fail "L never entered its critical section — the falsifier did not exercise the BKL-held path"
echo "[smp3] FALSIFIER: SMP-NO-DEADLOCK DEADLOCKED (no PASS, boot wedged in the BKL-held"
echo "       switch; L acquired the BKL then the mid-crit switch stranded it)"
echo "       => the §5.4 BKL-held reschedule guard is CERTIFIED load-bearing"
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
# Hard-pinned base .text sha (cf307b0e / f44aac86 line; the ②.2b deadlock work is
# ENTIRELY SMP_SELFTEST-gated, so the DEFAULT build MUST stay byte-identical).
SMP_BASE_SHA="755a20fae2d9b7415045193ea8287623dbeb906963609a63dce8a19c8a130513"
if [ "$DEF_SHA" != "$SMP_BASE_SHA" ]; then
    fail "DEFAULT .text sha $DEF_SHA != base $SMP_BASE_SHA — ②.2b deadlock work leaked into the shipped kernel!"
fi
echo "SMP-UNIPROC-SEMANTICS: PASS (.text byte-identical to base sha $SMP_BASE_SHA)"
# Optional extra cmp vs a base .text binary if the caller pins SMP_BASE_REF.
if [ -n "${SMP_BASE_REF:-}" ] && [ -f "${SMP_BASE_REF}" ]; then
    cmp -s "${SMP_BASE_REF}" /tmp/smp3_def_text.bin \
        || fail "DEFAULT .text DIFFERS from base binary ${SMP_BASE_REF}"
    echo "SMP-UNIPROC-SEMANTICS: also byte-identical to base binary ${SMP_BASE_REF}"
fi
echo

rm -f "$PASS_LOG" "$FAIL_LOG" "$DL_LOG" "$DLF_LOG"
echo "=== ②.2b-i [smp-async-preempt] + falsifier + [smp-uniproc-semantics] : ALL PASS ==="
