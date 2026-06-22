#!/bin/bash
# ---------------------------------------------------------------------------
# run_ss4.sh — host cert for SS-4 function-preserving expert growth
#              (arch/common/llm/student.c, ss4-function-preserving-growth-plan.md
#              §1/§4).
#
# SS-4 grows the student MoE's expert count m->nexpert by ADDING DEAD experts
# (router row=0, W2=0, alive[]=0).  This is EXACTLY function-preserving: the
# output of st_forward is BYTE-IDENTICAL for every input after the grow (ε=0),
# because the alive[]=-inf mask makes a DEAD expert PROVABLY never enter the
# chosen top-K set (so nk and every tw[j] are bit-unchanged).
#
# This router is top-K-then-softmax-over-the-chosen-set with MARGIN WIDENING,
# NOT a global softmax — so the textbook "clone an expert + half its router
# score" recipe is WRONG (a clone is admitted by the widening, steals softmax
# mass, shifts nk).  The DEAD + alive-mask transform is the exact fix.
#
# Certs (all IN-PROCESS, no network):
#   [expert-growth-preserves]  train M (E=4), hash logits[n*V], grow to E=8
#                              (DEAD), hash again -> BYTE-IDENTICAL (ε=0) AND the
#                              per-token firing widths nk are unchanged (the new
#                              experts truly never fired — non-vacuity).
#   [grow-noop-identity]       grow by ZERO experts -> hash + nk unchanged (the
#                              all-alive router path is byte-clean).
#   [grow-cohort]              a grown E=8 blob is REFUSED by an ungrown E=4
#                              model (distinct (tier,nexpert) cohort) and ACCEPTED
#                              by another E=8 model.
#   [no-vla]                   grep tripwire over student.c (st_grow_experts adds
#                              no runtime-sized stack array).
#
# FALSIFIER (-DSS4_GROW_NAIVE): the freshly-grown experts are made NAIVE LIVE
# random-init (alive=1, router row + W2 = random) — the textbook-wrong path.
# The new experts then enter the chosen set, nk shifts, the post-grow hash
# DIFFERS, and the cert FAILS (goes RED) deterministically.  The script asserts
# the falsifier build RETURNS NON-ZERO (proving the test has teeth).
#
#   ./run_ss4.sh
# Exit 0 = the DEAD cert PASSES and the NAIVE falsifier goes RED.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

SRC_TEST="$HERE/student_growth_test.c"
SRC_STU="$ROOT/arch/common/llm/student.c"

echo "[build] SS-4 cert (-O1 -ffp-contract=off -Werror=vla) ..."
$CC $CFLAGS "$SRC_TEST" "$SRC_STU" -o "$WORK/ss4" \
    || { echo "[build] FAILED"; exit 1; }

echo "[build] SS-4 FALSIFIER (-DSS4_GROW_NAIVE) ..."
$CC $CFLAGS -DSS4_GROW_NAIVE "$SRC_TEST" "$SRC_STU" -o "$WORK/ss4_naive" \
    || { echo "[build] FALSIFIER FAILED"; exit 1; }

# ---- [no-vla]: grep tripwire over student.c (same family as run_ss6.sh) ----
echo ""
echo "[no-vla] assert no stack array sized by a runtime dim in student.c"
VLA_HITS="$(grep -nE '\b(float|int|uint8_t|int8_t|double|char)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*\[(D|E|L|DFF|n|e_new|E_old|m->d|m->dff|m->nlayer|m->nexpert|m->n_params)\]' \
    "$SRC_STU" || true)"
if [ -n "$VLA_HITS" ]; then
    echo "  FAIL  [no-vla] a stack array is sized by a runtime dim:"
    echo "$VLA_HITS" | sed 's/^/      /'
    VLA_RC=1
else
    echo "  PASS  [no-vla] no stack array in student.c sized by D/E/L/DFF/n/e_new/m->*"
    echo "        (st_grow_experts reshards the HEAP arena; -Werror=vla is clean)"
    VLA_RC=0
fi

# ---- run the DEAD cert (must PASS) ----
echo ""
echo "[run] SS-4 in-process cert (DEAD expert growth) ..."
"$WORK/ss4"
CERT_RC=$?

echo ""
echo "[machine] DEAD growth (expect MATCH):"
"$WORK/ss4" --machine | sed 's/^/    /'

# ---- run the FALSIFIER (must go RED == non-zero) ----
echo ""
echo "[run] SS-4 FALSIFIER (-DSS4_GROW_NAIVE) — must go RED ..."
"$WORK/ss4_naive" >/dev/null 2>&1
NAIVE_RC=$?
echo "[machine] NAIVE growth (expect MISMATCH -> cert FAILS):"
"$WORK/ss4_naive" --machine | sed 's/^/    /'
if [ "$NAIVE_RC" -ne 0 ]; then
    echo "  PASS  [falsifier] the naive random-init grow makes the cert FAIL"
    echo "        (rc=$NAIVE_RC) — the byte-identity test has teeth"
    FALS_RC=0
else
    echo "  FAIL  [falsifier] the naive grow did NOT fail the cert (rc=0) —"
    echo "        the test is VACUOUS; STOP"
    FALS_RC=1
fi

echo ""
if [ "$CERT_RC" -eq 0 ] && [ "$VLA_RC" -eq 0 ] && [ "$FALS_RC" -eq 0 ]; then
    echo "[result] PASS"
    echo "[note] EXACT preservation is the ADD-DEAD-expert event only. Turning an"
    echo "       expert ON (resurrection) is deliberately ε-perturbing under the"
    echo "       margin-widening router and is a SEPARATE, looser [grow-then-learn]"
    echo "       cert — DEFERRED (ss4-function-preserving-growth-plan.md §2)."
    exit 0
else
    echo "[result] FAIL (cert=$CERT_RC novla=$VLA_RC falsifier=$FALS_RC)"
    exit 1
fi
