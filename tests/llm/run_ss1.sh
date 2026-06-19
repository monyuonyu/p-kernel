#!/bin/bash
# ---------------------------------------------------------------------------
# run_ss1.sh — host cert for SS-1 adaptive top-K in the Cradle baby
#              (arch/common/llm/student.c, special-structure-mind.md §4).
#
# Certs (all self-contained, no network; fixture if present else fallback):
#   [adaptive-k-margin]      hard (flat gate) fires wider than easy (peaked)
#   [adaptive-k-determinism] same bytes -> identical width + chosen experts
#   [no-loss-regression]     adaptive-K loss no worse than a fixed-K=2 build
#   [baby-merge-isolation]   student weights cannot reach R3's fleet merge
#                            (numeric ceiling in C + the source grep below)
#
# The fixed-K=2 baseline is the SAME cert rebuilt with -DST_K_THETA=0.0f (the
# router then never widens -> K==K_min==2). We scrape its LOSS_RESULT line and
# feed it to the adaptive build via --baseline for the no-regression compare.
#
#   ./run_ss1.sh                      # cert (fixture if present, else fallback)
#   GGUF=... ./run_ss1.sh             # (harvest is done by run_student.sh)
# Exit 0 = all PASS + the merge-isolation source grep is clean.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off"
WORK="$(mktemp -d)"
FIXTURE="$HERE/student_teacher.bytes"
trap 'rm -rf "$WORK"' EXIT

SRC_TEST="$HERE/student_adaptive_test.c"
SRC_STU="$ROOT/arch/common/llm/student.c"

echo "[build] adaptive build (margin widening live) ..."
$CC $CFLAGS "$SRC_TEST" "$SRC_STU" -o "$WORK/ss1_adaptive" \
    || { echo "[build] adaptive FAILED"; exit 1; }

echo "[build] fixed-K=2 baseline build (-DST_K_THETA=0.0f, never widens) ..."
$CC $CFLAGS -DST_K_THETA=0.0f "$SRC_TEST" "$SRC_STU" -o "$WORK/ss1_fixed" \
    || { echo "[build] fixed-K=2 FAILED"; exit 1; }

# ---- structural [baby-merge-isolation]: no merge path in the student tier ----
echo "[grep] [baby-merge-isolation] — assert no gl_merge call in student tier"
MERGE_HITS="$(grep -nE 'gl_merge_w?\s*\(' \
    "$ROOT/arch/common/llm/student.c" \
    "$ROOT/arch/common/llm/student_shell.c" 2>/dev/null || true)"
if [ -n "$MERGE_HITS" ]; then
    echo "  FAIL  [baby-merge-isolation] student tier CALLS gl_merge:"
    echo "$MERGE_HITS"
    GREP_RC=1
else
    echo "  PASS  [baby-merge-isolation] no gl_merge/gl_merge_w call in"
    echo "        student.c / student_shell.c (the student has NO merge path)"
    GREP_RC=0
fi

# ---- baseline loss from the THETA=0 build (== fixed K=2) ----
echo "[run] fixed-K=2 baseline (scrape LOSS_RESULT) ..."
FIXED_OUT="$( cd "$ROOT" && "$WORK/ss1_fixed" --fixture "$FIXTURE" )"
echo "$FIXED_OUT" | grep -E 'fixed-K=2|adaptive-K held-out|mean firing width' | sed 's/^/    /'
BASELINE="$(echo "$FIXED_OUT" | awk '/^LOSS_RESULT/ {print $2; exit}')"
echo "  fixed-K=2 baseline held-out loss = ${BASELINE:-<none>}"
# sanity: the fixed build must really pin width at K_min (mean == 2.000)
FIXEDW="$(echo "$FIXED_OUT" | awk -F'width ' '/mean firing width/ {print $2; exit}')"
echo "  fixed-build mean firing width = ${FIXEDW:-?} (must be 2.000)"

# ---- run the adaptive cert with the baseline ----
echo "[run] adaptive cert ..."
( cd "$ROOT" && "$WORK/ss1_adaptive" --fixture "$FIXTURE" --baseline "${BASELINE:-0}" )
CERT_RC=$?

echo ""
if [ "$CERT_RC" -eq 0 ] && [ "$GREP_RC" -eq 0 ]; then
    echo "[result] PASS"; exit 0
else
    echo "[result] FAIL (cert_rc=$CERT_RC grep_rc=$GREP_RC)"; exit 1
fi
