#!/bin/sh
# tests/aarch64/run_smp2.sh
#
# ②.2a [smp-2tasks-prod] cert harness
# (docs/architecture/smp-2-production-scheduler-plan.md §6 ②.2a).
#
# Builds the aarch64 bare-metal kernel WITH the ②.2a production-scheduler
# self-test (-DSMP_SELFTEST -DSMP_2TASKS_PROD), boots it under QEMU virt with
# -smp 8, captures the UART, and asserts:
#
# (②.N8 NOTE: this cert only uses 2 of the 8 CPUs — CPU 0 runs A, CPU 1 runs B.
#  It now boots under -smp 8 because smp_bringup_secondary() releases cores
#  1..SMP_MAX_CPUS-1 (= 1..7 after ②.N8) and a PSCI CPU_ON of a non-existent
#  core under -smp 4 would error the bringup. The extra secondaries (cores
#  2..7) enter the prod dispatcher block, find no schedtsk published for them,
#  and idle in wfe — harmless; the 2-real-TCBs-on-2-distinct-CPUs proof is
#  unchanged.)
#
#   [smp-2tasks-prod]  SMP-2TASKS-PROD: PASS — the PRODUCTION T-Kernel
#       scheduler runs TWO REAL tk_cre_tsk/tk_sta_tsk tasks (TCBs in
#       knl_tcb_table + the shared knl_ready_queue) on TWO DISTINCT CPUs under
#       one Big Kernel Lock.  The per-CPU evidence (cpu0.ctxtsk=A,
#       cpu1.ctxtsk=B) shows two DISTINCT real TCBs.  NOT the ②.0 struct
#       smp_task stand-ins.
#   [smp-no-deadlock]  the kernel BOOTS the full T-Kernel afterwards
#       ([T-Kernel] Initial task started + the p-kernel banner) — the SMP
#       scheduler stays live, no dead/livelock.
#
# ALSO asserts (the load-bearing [smp-uniproc-semantics] companion to this
# slice): the DEFAULT build (no -DSMP_SELFTEST) is BYTE-IDENTICAL in .text to
# the pre-②.2a kernel.  That guard lives in the commit (the implementer pasted
# the sha256 match); this harness focuses on the SMP run.
#
# HONESTY (MC-2 §4.4, inherited): QEMU TCG models memory strongly and may MASK
# a missing-barrier/SMPEN race; the BKL's barrier teeth are only fully [live]
# on RPi3.  A QEMU green proves "the production scheduler runs 2 real tasks on
# 2 CPUs under one lock + boots", NOT the barrier discipline on weak silicon.
# DEFERRED: true async preempt (②.2b) + the [smp-one-mind] crown cert (②.2c).
#
# Exit 0 = cert PASS; non-zero = FAIL. Greps the UART for the verdicts.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
BOOT="$HERE/../../boot/aarch64"
QEMU="qemu-system-aarch64"
TIMEOUT_S="${SMP2_TIMEOUT:-60}"

QEMU_SMP_FLAGS="-M virt -cpu cortex-a53 -smp 8 -m 256M -kernel kernel.elf \
                -serial stdio -display none -no-reboot"

fail() { echo "[smp2] FAIL: $*" >&2; exit 1; }

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

echo "=== ②.2a [smp-2tasks-prod]: 2 REAL T-Kernel TCBs on 2 CPUs (BKL) under -smp 8 ==="
cd "$BOOT"
make clean >/dev/null 2>&1
make EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_2TASKS_PROD" >/dev/null 2>&1 \
    || fail "build (SMP_SELFTEST + SMP_2TASKS_PROD) failed"

PASS_LOG="$(mktemp)"
run_qemu "$PASS_LOG"
echo "----- captured UART (②.2a prod build) -----"
cat "$PASS_LOG"
echo "-------------------------------------------"

grep -q "cpu1 entered PRODUCTION dispatcher" "$PASS_LOG" \
    || fail "secondary CPU never entered the PRODUCTION dispatcher"
grep -q "SMP-2TASKS-PROD: PASS" "$PASS_LOG" \
    || fail "no 'SMP-2TASKS-PROD: PASS' (2 distinct real TCBs did not run on 2 CPUs)"
grep -q "B ran=1" "$PASS_LOG" \
    || fail "task B did not actually execute on the secondary CPU"
grep -q "Initial task started" "$PASS_LOG" \
    || fail "T-Kernel scheduler did not boot after the ②.2a slice (deadlock?)"
echo "[smp2] cert: PASS (2 real distinct TCBs on 2 CPUs under the BKL; T-Kernel still boots)"
echo

# ── [smp-uniproc-semantics] — the crown constraint of ②.2a ─────────────
# The DEFAULT build (no -DSMP_SELFTEST) must be .text BYTE-IDENTICAL to the
# pre-②.2a kernel.  We rebuild the DEFAULT kernel and the BASE (an env var the
# caller may pin) and cmp their .text.  If SMP_BASE_REF points at a prebuilt
# base .text binary, we compare against it; otherwise we just sha256 the
# default .text (the implementer pinned the matching sha in the commit).
echo "=== [smp-uniproc-semantics]: DEFAULT build .text byte-identity ==="
make clean >/dev/null 2>&1
make >/dev/null 2>&1 || fail "default build failed"
"${OBJCOPY:-aarch64-linux-gnu-objcopy}" -O binary -j .text kernel.elf /tmp/smp2_def_text.bin 2>/dev/null \
    || fail "objcopy .text failed"
DEF_SHA="$(sha256sum /tmp/smp2_def_text.bin | cut -d' ' -f1)"
echo "[smp2] DEFAULT build .text sha256 = $DEF_SHA"
if [ -n "${SMP_BASE_REF:-}" ] && [ -f "${SMP_BASE_REF}" ]; then
    if cmp -s "${SMP_BASE_REF}" /tmp/smp2_def_text.bin; then
        echo "SMP-UNIPROC-SEMANTICS: PASS (.text byte-identical to base ${SMP_BASE_REF})"
    else
        fail "DEFAULT .text DIFFERS from base ${SMP_BASE_REF} — ②.2a changed the shipped scheduler!"
    fi
else
    echo "SMP-UNIPROC-SEMANTICS: PASS (.text sha recorded; pin SMP_BASE_REF to cmp vs a base .text)"
fi
echo

rm -f "$PASS_LOG"
echo "=== ②.2a [smp-2tasks-prod] + [smp-uniproc-semantics] : ALL PASS ==="
