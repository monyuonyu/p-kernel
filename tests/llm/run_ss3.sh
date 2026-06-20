#!/bin/bash
# ---------------------------------------------------------------------------
# run_ss3.sh — host cert for SS-3 same-tier merge cohorts in the Cradle baby
#              (arch/common/llm/student.c, special-structure-mind.md §3.2/§8.4).
#
# Certs (all self-contained, IN-PROCESS — the core claim needs NO network):
#   [ss3-cohort-converge]   two M-tier students seeded identically, trained on a
#                           SHARED objective with different data orderings, then
#                           merged same-tier: merged held-out loss <= the worse
#                           parent AND merge is peer-symmetric (merge(A,{B})
#                           bytes == merge(B,{A}) bytes).
#   [ss3-cohort-island]     an S-tier blob offered to an M-tier merge is REFUSED
#                           (accepted==0) and the M weights are byte-unchanged.
#   [ss3-merge-falsifiable] a 1e-3-perturbed peer BREAKS the symmetry (the cert
#                           can FAIL — falsifiability like the KV cert).
#   [baby-merge-isolation]  (re-confirmed here) NO gl_merge / gl_merge_w call in
#                           the student tier — st_merge_cohort is its OWN math.
#   [no-vla]                no stack array in the SS-3 merge path is sized by a
#                           runtime dim (count / n_params / m->*).
#
# HONESTY (Path-W, memory moment_2026_06_12_wave41_one_mind): averaging is the
# MECHANISM and converges minds toward a SHARED objective; it is LOSSY for two
# minds that learned DIFFERENT facts. This cert proves the mechanism, NOT
# divergent-fact preservation (that is Path-W^2, out of scope).
#
#   ./run_ss3.sh
# Exit 0 = all certs PASS + the isolation/no-vla greps are clean.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

SRC_TEST="$HERE/student_cohort_test.c"
SRC_STU="$ROOT/arch/common/llm/student.c"

echo "[build] SS-3 cohort cert (student.c + st_merge_cohort) ... (-Werror=vla)"
$CC $CFLAGS "$SRC_TEST" "$SRC_STU" -o "$WORK/ss3" \
    || { echo "[build] FAILED"; exit 1; }

# ---- structural [baby-merge-isolation]: no R3 merge in the student tier ----
# st_merge_cohort is the student's OWN averaging math; it must NEVER reach R3's
# gl_merge / gl_merge_w (the fleet crown over rw[]).
echo ""
echo "[grep] [baby-merge-isolation] — assert no gl_merge call in student tier"
MERGE_HITS="$(grep -nE 'gl_merge_w?\s*\(' \
    "$ROOT/arch/common/llm/student.c" \
    "$ROOT/arch/common/llm/student_shell.c" 2>/dev/null || true)"
if [ -n "$MERGE_HITS" ]; then
    echo "  FAIL  [baby-merge-isolation] student tier CALLS gl_merge:"
    echo "$MERGE_HITS"
    GREP_RC=1
else
    echo "  PASS  [baby-merge-isolation] no gl_merge/gl_merge_w in student.c /"
    echo "        student_shell.c — st_merge_cohort is the student's OWN math"
    GREP_RC=0
fi

# ---- [no-vla]: no stack array sized by a runtime dim in the merge path ----
# The SS-3 merge reduces with two int loop counters and reads peers via a byte
# copy; the ONLY arrays it uses are caller-provided heap blobs + the fixed
# ST_BLOB_HDR scratch. Reuse the SS-2 grep over student.c (covers st_merge_cohort
# + st_blob_tier_ok + st_blob_w_at). -Werror=vla above is the compiler backstop.
echo ""
echo "[no-vla] assert no stack array sized by a runtime dim in student.c"
VLA_HITS="$(grep -nE '\b(float|int|uint8_t|double|char)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*\[(D|E|L|DFF|n|np|count|m->d|m->dff|m->nlayer|m->nexpert|m->n_params)\]' \
    "$SRC_STU" || true)"
if [ -n "$VLA_HITS" ]; then
    echo "  FAIL  [no-vla] a stack array is sized by a runtime dim:"
    echo "$VLA_HITS" | sed 's/^/      /'
    VLA_RC=1
else
    echo "  PASS  [no-vla] no stack array in student.c sized by a runtime dim"
    echo "        (merge uses int counters + heap blobs; -Werror=vla is clean)"
    VLA_RC=0
fi

# ---- run the in-process cohort cert ----
echo ""
echo "[run] SS-3 in-process cohort cert ..."
"$WORK/ss3"
CERT_RC=$?

echo ""
if [ "$CERT_RC" -eq 0 ] && [ "$GREP_RC" -eq 0 ] && [ "$VLA_RC" -eq 0 ]; then
    echo "[result] PASS"
    echo "[note] live-network chunked publish/fetch (gl_student_publish /"
    echo "       gl_student_fetch) is a DEFERRED [live] row — the converge claim"
    echo "       above is in-process and needs no relay."
    exit 0
else
    echo "[result] FAIL (cert=$CERT_RC isolation=$GREP_RC novla=$VLA_RC)"
    exit 1
fi
