#!/bin/bash
# ---------------------------------------------------------------------------
# run_devfit.sh — host cert for DEVICE-CAPACITY mind-sizing (DEVFIT-1).
#                 (arch/common/llm/dev_capacity.c + student.c,
#                  docs/architecture/device-capacity-mind-sizing-plan.md)
#
# mk_pino's explicit direction: 「デバイスのスペックを測って自動で合わせたい」.
# The SAME student binary, given different INJECTED device profiles, sizes its
# student to the matching tier (S/M/L) and REFUSES to OOM a small device. This
# is the mind-sizing analogue of SMP-AUTODETECT's "same binary, -smp 2/4/8".
#
# Certs (in-process, fixture-injected — no real hardware):
#   [device-fit]            each profile -> matching tier + arena fits (RAM is
#                           the bottleneck: 512MB/8-core -> S, not L). Thresholds
#                           are the COMPUTED per-tier arena cost (non-vacuous).
#   [device-fit-monotone]   sweep RAM up -> tier non-decreasing.
#   [device-fit-ceiling]    SS-4 reconciliation: a big fleet on an S device
#                           clamps to ST_E_S=2, NOT 16 (open-risk #7).
#   [tier-forward-pin]      S=0a5bf44c.. (NEWLY PINNED), M=63e8de33.., L=67f2434f..
#                           UNMOVED (same recipe as run_ss6.sh single= hashes).
#
# THE FALSIFIER (-DDEVFIT_IGNORE_MEASURE, the load-bearing control): a driver
# that calls st_init_device under a CONSTRAINED address space (ulimit -v) so the
# ~314MB L arena cannot allocate. The PRODUCTION build (with measurement +
# step-down) sizes the 512MB profile to S (~2.5MB) and prints OK; the FALSIFIER
# build (hardcoded L, NO step-down) tries the L arena and OOMs. The script
# asserts production==OK and falsifier==OOM -> the measurement is load-bearing.
#
#   ./run_devfit.sh
# Exit 0 = all certs PASS, the no-vla grep is clean, AND the falsifier goes RED.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

SRC_TEST="$HERE/student_devfit_test.c"
SRC_FALS="$HERE/student_devfit_falsify.c"
SRC_STU="$ROOT/arch/common/llm/student.c"
SRC_DEV="$ROOT/arch/common/llm/dev_capacity.c"

echo "[build] DEVFIT cert (-O1 -ffp-contract=off -Werror=vla) ..."
$CC $CFLAGS "$SRC_TEST" "$SRC_STU" "$SRC_DEV" -o "$WORK/devfit" \
    || { echo "[build] FAILED"; exit 1; }

echo "[build] DEVFIT falsifier driver (production + -DDEVFIT_IGNORE_MEASURE) ..."
$CC $CFLAGS "$SRC_FALS" "$SRC_STU" "$SRC_DEV" -o "$WORK/fals_prod" \
    || { echo "[build] FALS prod FAILED"; exit 1; }
$CC $CFLAGS -DDEVFIT_IGNORE_MEASURE "$SRC_FALS" "$SRC_STU" "$SRC_DEV" -o "$WORK/fals_ign" \
    || { echo "[build] FALS ignore FAILED"; exit 1; }

# ---- [no-vla]: grep tripwire over dev_capacity.c (same family as run_ss6) ----
echo ""
echo "[no-vla] assert no stack array sized by a runtime dim in dev_capacity.c"
VLA_HITS="$(grep -nE '\b(float|int|uint8_t|int8_t|double|char)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*\[(D|E|L|DFF|n|tier|m->d|m->dff|m->nlayer|m->nexpert)\]' \
    "$SRC_DEV" || true)"
if [ -n "$VLA_HITS" ]; then
    echo "  FAIL  [no-vla] a stack array is sized by a runtime dim:"
    echo "$VLA_HITS" | sed 's/^/      /'
    VLA_RC=1
else
    echo "  PASS  [no-vla] dev_capacity.c sizes no per-tier stack array (it only"
    echo "        picks a tier byte; the student keeps its ST_*_MAX scratch bound)"
    VLA_RC=0
fi

# ---- run the cert (must PASS) ----
echo ""
echo "[run] DEVFIT in-process cert ..."
"$WORK/devfit"
CERT_RC=$?

echo ""
echo "[machine] profile -> tier table + forward hashes:"
"$WORK/devfit" --machine | sed 's/^/    /'

# ---- the FALSIFIER: 512MB profile, address space capped below the L arena ----
# The L arena is ~314MB (w|g|mu|vu). Cap virtual memory at 256MB so L cannot
# allocate but S (~2.5MB) / M (~29MB) can. Inject the 512MB profile.
echo ""
echo "[falsifier] 512MB profile under a ~256MB address-space cap (ulimit -v):"
CAP_KB=262144   # 256 MB
PROD_OUT="$(PKERNEL_DEVICE_RAM_BYTES=536870912 PKERNEL_DEVICE_CORES=2 PKERNEL_DEVICE_RAM_TRUST=2 \
            bash -c "ulimit -v $CAP_KB 2>/dev/null; '$WORK/fals_prod'" 2>&1)"
PROD_RC=$?
IGN_OUT="$(PKERNEL_DEVICE_RAM_BYTES=536870912 PKERNEL_DEVICE_CORES=2 PKERNEL_DEVICE_RAM_TRUST=2 \
            bash -c "ulimit -v $CAP_KB 2>/dev/null; '$WORK/fals_ign'" 2>&1)"
IGN_RC=$?
echo "    PRODUCTION (measure + step-down) : rc=$PROD_RC  $PROD_OUT"
echo "    FALSIFIER  (-DDEVFIT_IGNORE_MEASURE): rc=$IGN_RC  $IGN_OUT"

# production must bring up a FITTING student (rc 0); falsifier must OOM (rc != 0).
if [ "$PROD_RC" -eq 0 ] && [ "$IGN_RC" -ne 0 ]; then
    echo "  PASS  [falsifier] ignoring the measurement OOMs the small device,"
    echo "        the measured+step-down build fits it — measurement is LOAD-BEARING"
    FALS_RC=0
else
    echo "  FAIL  [falsifier] expected production=OK + falsifier=OOM"
    echo "        (got prod_rc=$PROD_RC ign_rc=$IGN_RC) — the cert is VACUOUS; STOP"
    FALS_RC=1
fi

echo ""
if [ "$CERT_RC" -eq 0 ] && [ "$VLA_RC" -eq 0 ] && [ "$FALS_RC" -eq 0 ]; then
    echo "[result] PASS"
    echo "[note] HONEST scope: boot-time sizing ONLY (no runtime re-tiering);"
    echo "       device-sizing fragments the fleet into <=3 student cohorts that"
    echo "       share the R3 crown unconditionally + merge WITHIN a tier — the"
    echo "       cross-cohort distillation bridge is NOT solved here."
    exit 0
else
    echo "[result] FAIL (cert=$CERT_RC novla=$VLA_RC falsifier=$FALS_RC)"
    exit 1
fi
