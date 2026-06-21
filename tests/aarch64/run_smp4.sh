#!/bin/sh
# tests/aarch64/run_smp4.sh
#
# ②.2c [smp-one-mind] CROWN cert harness
# (docs/architecture/smp-2c-one-mind-plan.md).
#
# THE PAYOFF of the entire ② full-SMP arc. Builds the aarch64 bare-metal kernel
# WITH the ②.2c crown self-test (-DSMP_SELFTEST -DSMP_ONE_MIND), boots it under
# QEMU virt with -smp 4, captures the UART, and asserts:
#
#   [smp-one-mind]  SMP-ONE-MIND: PASS — a REAL bare-metal mind forward
#       (r_forward, the R3 in-context Transformer) is BYTE-IDENTICAL whether it
#       runs (a) under the shipped uniprocessor path (H_uni, on CPU 0) or (b) as
#       a real T-Kernel task ON A SECONDARY CPU under the live SMP scheduler
#       (H_smp, on CPU 1, via the ②.2a smp_prod pattern, while CPUs 2,3 run busy
#       filler — genuine concurrency).  H_uni == H_smp ⇒ the SMP scheduler did
#       not perturb a single bit of the mind's math.  "The mind stays one across
#       the SMP scheduler."  The PASS hash is STABLE + EQUAL across 3 runs.
#
# Then builds the FALSIFIER (+ -DSMP_ONEMIND_RACE) and asserts it goes RED:
#   SMP-ONE-MIND: FAIL hashes-differ — a SECOND CPU scribbles the SHARED static
#   rc/rw[] of the mind forward WITHOUT serialization while CPU 1's forward is
#   mid-flight → the activations are corrupted → H_smp != H_uni.  Proves the cert
#   observes the REAL mind output AND that the single-forward-at-a-time discipline
#   is LOAD-BEARING (not vacuously passing because r_forward is single-threaded).
#
# ALSO asserts the DEFAULT build (no -DSMP_SELFTEST) is .text BYTE-IDENTICAL to
# base (the SMP_BASE_SHA pin; the ②.2c work is ENTIRELY SMP_ONE_MIND-gated).
#
# HONEST NARROWING (stated in the cert, the commit, the doc): the crown proves a
# SINGLE forward survives SMP scheduling bit-for-bit.  CONCURRENT mind operations
# (two forwards at once / forward-while-train) race the shared rc/rw[] and need a
# mind-lock — DEFERRED (that is exactly what the falsifier exploits).  ②.2b-ii
# (secondary timer/WAIT) is NOT needed: the forward is run-to-completion + parks
# on wfe.
#
# HONESTY (QEMU vs HW): a QEMU -smp 4 PASS proves the scheduler did not
# reorder/corrupt the mind's math under real concurrency (the determinism).  It
# does NOT prove the BKL/SGI barrier/cache-coherency discipline on weakly-ordered
# silicon (QEMU TCG models memory strongly) — that is [live]-only on RPi3.
#
# Exit 0 = cert PASS; non-zero = FAIL. Greps the UART for the verdicts.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
BOOT="$HERE/../../boot/aarch64"
QEMU="qemu-system-aarch64"
TIMEOUT_S="${SMP4_TIMEOUT:-60}"

QEMU_SMP_FLAGS="-M virt -cpu cortex-a53 -smp 4 -m 256M -kernel kernel.elf \
                -serial stdio -display none -no-reboot"

fail() { echo "[smp4] FAIL: $*" >&2; exit 1; }

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

# Extract "H_uni=0x..." / "H_smp=0x..." from a PASS-line log.
extract_hash() {   # $1=log  $2=H_uni|H_smp
    grep -ao "$2=0x[0-9a-f]*" "$1" | head -1 | cut -d= -f2
}

# ── PASS build (run 3x; the hash must be STABLE + EQUAL every run) ───────
echo "=== ②.2c [smp-one-mind] CROWN: r_forward byte-identical uniproc vs SMP-secondary (-smp 4) ==="
cd "$BOOT"
make clean >/dev/null 2>&1
make EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_ONE_MIND" >/dev/null 2>&1 \
    || fail "build (SMP_SELFTEST + SMP_ONE_MIND) failed"

PREV_HASH=""
i=1
while [ "$i" -le 3 ]; do
    PASS_LOG="$(mktemp)"
    run_qemu "$PASS_LOG"
    echo "----- captured UART (②.2c one-mind build, run $i) -----"
    cat "$PASS_LOG"
    echo "-------------------------------------------------------"

    grep -aq "SMP-ONE-MIND: PASS" "$PASS_LOG" \
        || fail "run $i: no 'SMP-ONE-MIND: PASS' (the mind was perturbed by SMP scheduling, or M did not run on CPU 1)"
    grep -aq "M ran=1" "$PASS_LOG" \
        || fail "run $i: the mind task M did not run on the secondary CPU"
    grep -aq "Initial task started" "$PASS_LOG" \
        || fail "run $i: T-Kernel did not boot after the crown cert (deadlock / BKL strand?)"

    HU="$(extract_hash "$PASS_LOG" H_uni)"
    HS="$(extract_hash "$PASS_LOG" H_smp)"
    [ -n "$HU" ] && [ -n "$HS" ] || fail "run $i: could not parse H_uni/H_smp from the PASS line"
    [ "$HU" = "$HS" ] || fail "run $i: H_uni ($HU) != H_smp ($HS) on a PASS line — split mind!"
    echo "[smp4] run $i: H_uni == H_smp == $HU"
    if [ -n "$PREV_HASH" ] && [ "$HU" != "$PREV_HASH" ]; then
        fail "the hash is NOT STABLE across runs ($PREV_HASH vs $HU) — the forward is non-deterministic"
    fi
    PREV_HASH="$HU"
    rm -f "$PASS_LOG"
    i=$((i + 1))
done
echo "[smp4] PASS build: H_uni == H_smp == $PREV_HASH, STABLE across 3 runs"
echo "       => a single bare-metal r_forward is byte-identical uniproc vs scheduled on an SMP secondary"
echo

# ── FALSIFIER build (-DSMP_ONEMIND_RACE) must go RED (run 3x) ────────────
echo "=== FALSIFIER -DSMP_ONEMIND_RACE (must FAIL: a 2nd CPU races the shared rc/rw[]) ==="
make clean >/dev/null 2>&1
make EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_ONE_MIND -DSMP_ONEMIND_RACE" >/dev/null 2>&1 \
    || fail "falsifier build (+ SMP_ONEMIND_RACE) failed"

i=1
while [ "$i" -le 3 ]; do
    FAIL_LOG="$(mktemp)"
    run_qemu "$FAIL_LOG"
    echo "----- captured UART (SMP_ONEMIND_RACE falsifier build, run $i) -----"
    cat "$FAIL_LOG"
    echo "-------------------------------------------------------------------"

    grep -aq "SMP-ONE-MIND: FAIL hashes-differ" "$FAIL_LOG" \
        || fail "run $i: the SMP_ONEMIND_RACE falsifier did NOT FAIL hashes-differ — the cert is vacuous (the shared-rc race did not perturb the mind?)"
    if grep -aq "SMP-ONE-MIND: PASS" "$FAIL_LOG"; then
        fail "run $i: the falsifier PASSED — the single-forward discipline is NOT load-bearing (cert vacuous)!"
    fi
    HU="$(extract_hash "$FAIL_LOG" H_uni)"
    HS="$(extract_hash "$FAIL_LOG" H_smp)"
    [ -n "$HU" ] && [ -n "$HS" ] || fail "run $i: could not parse the two differing hashes from the FAIL line"
    [ "$HU" != "$HS" ] || fail "run $i: the FAIL line shows EQUAL hashes ($HU) — not a real race"
    echo "[smp4] run $i: FALSIFIER differing hashes  H_uni=$HU  H_smp=$HS"
    rm -f "$FAIL_LOG"
    i=$((i + 1))
done
echo "[smp4] FALSIFIER: SMP-ONE-MIND: FAIL hashes-differ across 3 runs"
echo "       => the single-forward-at-a-time discipline is LOAD-BEARING (the cert is non-vacuous)"
echo

# ── DEFAULT build .text byte-identity ───────────────────────────────────
echo "=== [smp-uniproc-semantics]: DEFAULT build .text byte-identity ==="
make clean >/dev/null 2>&1
make >/dev/null 2>&1 || fail "default build failed"
"${OBJCOPY:-aarch64-linux-gnu-objcopy}" -O binary -j .text kernel.elf /tmp/smp4_def_text.bin 2>/dev/null \
    || fail "objcopy .text failed"
DEF_SHA="$(sha256sum /tmp/smp4_def_text.bin | cut -d' ' -f1)"
echo "[smp4] DEFAULT build .text sha256 = $DEF_SHA"
# Hard-pinned base .text sha (the same pin run_smp3.sh uses; ②.2c is ENTIRELY
# SMP_ONE_MIND-gated, so the DEFAULT build MUST stay byte-identical to base).
SMP_BASE_SHA="755a20fae2d9b7415045193ea8287623dbeb906963609a63dce8a19c8a130513"
if [ "$DEF_SHA" != "$SMP_BASE_SHA" ]; then
    fail "DEFAULT .text sha $DEF_SHA != base $SMP_BASE_SHA — ②.2c crown work leaked into the shipped kernel!"
fi
echo "SMP-UNIPROC-SEMANTICS: PASS (.text byte-identical to base sha $SMP_BASE_SHA)"
if [ -n "${SMP_BASE_REF:-}" ] && [ -f "${SMP_BASE_REF}" ]; then
    cmp -s "${SMP_BASE_REF}" /tmp/smp4_def_text.bin \
        || fail "DEFAULT .text DIFFERS from base binary ${SMP_BASE_REF}"
    echo "SMP-UNIPROC-SEMANTICS: also byte-identical to base binary ${SMP_BASE_REF}"
fi
echo

echo "=== ②.2c [smp-one-mind] CROWN + falsifier + [smp-uniproc-semantics] : ALL PASS ==="
echo "    The mind stays ONE across the SMP scheduler (single-forward scope; concurrent"
echo "    minds = a mind-lock, DEFERRED).  QEMU green != hardware green (barrier/coherency"
echo "    on weak silicon is [live]-only on RPi3)."
