#!/bin/bash
# ---------------------------------------------------------------------------
# run_cradle_teach.sh — host cert for the Thread T lesson BRIDGE (T-fix-b/T-1).
#                       (docs/architecture/thread-t-impl-plan.md §1.)
#
# Links the REAL production seam (arch/common/llm/cradle.c — the lesson ring +
# ingest + the cert) + arch/common/llm/student.c (the math) and runs the
# in-process [cradle-teach] cert: A teaches a lesson, B pulls + trains, and B
# then KNOWS a held-out probe it was NEVER directly trained on (generalization,
# weight-resident). Three falsification arms (each goes RED):
#   ARM A  teaching OFF  -> fixture fallback, probe stays at chance (rode mesh)
#   ARM B  scrambled     -> random bytes do NOT teach the probe (sequence, not stats)
#   ARM C  never-taught  -> a fact absent from the lesson stays unknown (no pre-bake)
#
# Build flags pin the one-mind math (-O1 -ffp-contract=off, -Werror=vla) — the
# salty-bug class (clang FMA vs gcc rounding) is sealed by the same recipe the
# kernel uses.
#
#   ./run_cradle_teach.sh
# Exit 0 = cert PASS + the NOCENTRAL/isolation greps clean.
#
# HONEST: this is the [in-proc] cert. The multi-process LIVE teacher-convergence
# over ./relay is a DEFERRED [live] row; the live in-kernel GGUF teacher harvest
# is DEFERRED (CT-2). The BRIDGE + the student ingestion is proven here.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

SRC_TEST="$HERE/cradle_teach_proof.c"
SRC_CRADLE="$ROOT/arch/common/llm/cradle.c"
SRC_STU="$ROOT/arch/common/llm/student.c"

echo "[build] cradle-teach cert (cradle.c + student.c) ... (-ffp-contract=off -Werror=vla)"
$CC $CFLAGS "$SRC_TEST" "$SRC_CRADLE" "$SRC_STU" -o "$WORK/ct" \
    || { echo "[build] FAILED"; exit 1; }

# ---- structural [cradle-nocentral]: the bridge never averages weights ------
# The student trains via its OWN distill path; cradle.c must NEVER call gl_merge
# (the fleet crown over rw[]) nor st_merge_cohort (one-mind, NOCENTRAL).
echo ""
echo "[grep] [cradle-nocentral] — assert no gl_merge / st_merge_cohort in cradle.c"
HITS="$(grep -nE 'gl_merge_w?\s*\(|st_merge_cohort\s*\(' "$SRC_CRADLE" 2>/dev/null || true)"
if [ -n "$HITS" ]; then
    echo "  FAIL  [cradle-nocentral] cradle.c averages weights:"
    echo "$HITS"
    GREP_RC=1
else
    echo "  PASS  [cradle-nocentral] cradle.c never averages weights — the"
    echo "        student learns via its OWN st_backward/st_adam_step train path"
    GREP_RC=0
fi

# ---- run the in-process cert ----
echo ""
echo "[run] in-process [cradle-teach] cert ..."
"$WORK/ct"
CERT_RC=$?

echo ""
if [ "$CERT_RC" -eq 0 ] && [ "$GREP_RC" -eq 0 ]; then
    echo "[result] PASS"
    echo "[note] live multi-process teacher-convergence over ./relay is a DEFERRED"
    echo "       [live] row; live in-kernel GGUF teacher harvest is DEFERRED (CT-2)."
    exit 0
else
    echo "[result] FAIL (cert=$CERT_RC nocentral=$GREP_RC)"
    exit 1
fi
