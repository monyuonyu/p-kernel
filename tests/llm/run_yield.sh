#!/bin/bash
# ---------------------------------------------------------------------------
# run_yield.sh — host cert for the COOPERATIVE-YIELD DMN fix
#                (docs/architecture/cooperative-yield-plan.md;
#                 arch/common/llm/student_shell.c).
#
# Builds the cert against the REAL production student_shell.c (#included so its
# static sliced-consolidation internals + the file-static cursor are exercised)
# plus the real student.c math, and runs:
#
#   [yield-byte-identical]  sliced (w/ interleaved pure-read inference) == the
#                           all-at-once run, FULL st_save blob memcmp == 0.
#   [yield-responsive]      the production student_dmn_consolidate() state
#                           machine advances <= K passes/call, the cumulative
#                           pass count is unchanged (not skipped), and the final
#                           blob/loss == the all-at-once reference.
#
# FALSIFIER: a second build with -DYIELD_DISABLE runs the whole batch per call.
# The cert MUST go RED there ([yield-responsive] (a) fails) — proof of teeth.
# This script PASSes only if the normal build is GREEN AND the falsifier is RED.
#
#   ./run_yield.sh
# Exit 0 = fix present & certified.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

SRC="$HERE/student_yield_test.c $ROOT/arch/common/llm/student.c"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off"

echo "[build] compiling cooperative-yield cert (fix ON) ..."
# shellcheck disable=SC2086
"$CC" $CFLAGS $SRC -lm -o "$WORK/yield_on" || { echo "[build] FAILED"; exit 1; }

echo "[build] compiling falsifier (-DYIELD_DISABLE) ..."
# shellcheck disable=SC2086
"$CC" $CFLAGS -DYIELD_DISABLE $SRC -lm -o "$WORK/yield_off" \
    || { echo "[build] falsifier FAILED"; exit 1; }

echo ""
echo "=== fix ON (must PASS) ==="
( cd "$ROOT" && "$WORK/yield_on" ); ON_RC=$?

echo ""
echo "=== falsifier -DYIELD_DISABLE (must FAIL = RED, proving teeth) ==="
( cd "$ROOT" && "$WORK/yield_off" ); OFF_RC=$?

echo ""
if [ "$ON_RC" -eq 0 ] && [ "$OFF_RC" -ne 0 ]; then
    echo "[result] PASS  (fix GREEN, falsifier RED — the cert has teeth)"
    exit 0
fi
echo "[result] FAIL  (fix rc=$ON_RC [want 0], falsifier rc=$OFF_RC [want non-0])"
exit 1
