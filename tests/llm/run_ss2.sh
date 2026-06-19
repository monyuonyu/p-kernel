#!/bin/bash
# ---------------------------------------------------------------------------
# run_ss2.sh — host cert for SS-2 tier-selectable model dims in the Cradle baby
#              (arch/common/llm/student.c, special-structure-mind.md §3.2).
#
# Certs (all self-contained, no network):
#   [m-identical]        the M-tier forward-logit FNV hash + held-out loss are
#                        BYTE-IDENTICAL between the NEW (tier-aware) student.c
#                        and the BASE (pre-SS-2) student.c.  This is the critical
#                        no-regression: M must reproduce the predecessor exactly.
#   [no-loss-regression] M-tier held-out loss == the base loss (same number).
#   [tier-load]          (new build) save M->load M ok; save S->load S ok;
#                        save M -> load S REFUSED (fail-closed).
#   [tier-distinct/run]  (new build) S/M/L distinct shapes, all forward, widths
#                        bounded by each tier's own E.
#   [no-vla]             grep tripwire: NO stack array in student.c is sized by
#                        a runtime dim (m->d/dff/nlayer/nexpert) — every scratch
#                        array is bound to a fixed ST_*_MAX / V / ST_MAXSEQ.
#
# The BASE student.c is the pre-SS-2 snapshot: it is taken from git (the SS-1
# tip, commit 5edbeeda) if available, else from $SS2_BASE_STUDENT.
#
#   ./run_ss2.sh
# Exit 0 = all certs PASS + the hashes match + the no-vla grep is clean.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

SRC_TEST="$HERE/student_tier_test.c"
SRC_NEW="$ROOT/arch/common/llm/student.c"

# ---- obtain the BASE (pre-SS-2) student.c + student.h ----
BASE_DIR="$WORK/base"
mkdir -p "$BASE_DIR"
BASE_REF="${SS2_BASE_REF:-5edbeeda}"   # the SS-1 tip (pre-SS-2)
if [ -n "${SS2_BASE_STUDENT:-}" ] && [ -f "${SS2_BASE_STUDENT}" ]; then
    cp "$SS2_BASE_STUDENT" "$BASE_DIR/student.c"
    cp "$(dirname "$SS2_BASE_STUDENT")/student.h" "$BASE_DIR/student.h"
    echo "[base] using SS2_BASE_STUDENT=$SS2_BASE_STUDENT"
elif git -C "$ROOT" cat-file -e "${BASE_REF}:arch/common/llm/student.c" 2>/dev/null; then
    git -C "$ROOT" show "${BASE_REF}:arch/common/llm/student.c" > "$BASE_DIR/student.c"
    git -C "$ROOT" show "${BASE_REF}:arch/common/llm/student.h" > "$BASE_DIR/student.h"
    echo "[base] extracted pre-SS-2 student from git ${BASE_REF}"
else
    echo "[base] FAILED to obtain a pre-SS-2 base student (ref=${BASE_REF})"
    exit 1
fi

# The base test must #include the BASE header. The test file uses a relative
# include "../../arch/common/llm/student.h"; for the base build we compile a
# copy of the test whose include points at the base header dir.
BASE_TEST="$WORK/base_test.c"
sed 's#"../../arch/common/llm/student.h"#"student.h"#' "$SRC_TEST" > "$BASE_TEST"

echo "[build] NEW  (tier-aware) student.c ..."
$CC $CFLAGS "$SRC_TEST" "$SRC_NEW" -o "$WORK/ss2_new" \
    || { echo "[build] NEW FAILED"; exit 1; }

echo "[build] BASE (pre-SS-2) student.c ..."
$CC $CFLAGS -I"$BASE_DIR" "$BASE_TEST" "$BASE_DIR/student.c" -o "$WORK/ss2_base" \
    || { echo "[build] BASE FAILED"; exit 1; }

# ---- [m-identical] + [no-loss-regression]: diff the M hash/loss ----
echo ""
echo "[m-identical] M-tier hash/loss: new vs base (pre-SS-2)"
NEW_OUT="$("$WORK/ss2_new" --machine)"
BASE_OUT="$("$WORK/ss2_base" --machine)"
NEW_HASH="$(echo "$NEW_OUT"  | awk '/^M_HASH/ {print $2; exit}')"
BASE_HASH="$(echo "$BASE_OUT" | awk '/^M_HASH/ {print $2; exit}')"
NEW_LOSS="$(echo "$NEW_OUT"  | awk '/^M_LOSS/ {print $2; exit}')"
BASE_LOSS="$(echo "$BASE_OUT" | awk '/^M_LOSS/ {print $2; exit}')"
echo "  new  hash=$NEW_HASH  loss=$NEW_LOSS"
echo "  base hash=$BASE_HASH  loss=$BASE_LOSS"
IDENT_RC=0
if [ "$NEW_HASH" = "$BASE_HASH" ] && [ -n "$NEW_HASH" ]; then
    echo "  PASS  [m-identical] M-tier forward-logit hash is byte-identical"
else
    echo "  FAIL  [m-identical] M-tier hash DIFFERS (regression!)"; IDENT_RC=1
fi
if [ "$NEW_LOSS" = "$BASE_LOSS" ] && [ -n "$NEW_LOSS" ]; then
    echo "  PASS  [no-loss-regression] M-tier held-out loss == base loss"
else
    echo "  FAIL  [no-loss-regression] M-tier loss DIFFERS"; IDENT_RC=1
fi

# ---- the tier/load certs (new build) ----
echo ""
echo "[run] new-build tier certs ..."
"$WORK/ss2_new"
CERT_RC=$?

# ---- [no-vla]: grep tripwire ----
# Every stack-local array in student.c must be sized by a FIXED bound:
# ST_*_MAX / KMAX / V / ST_VOCAB / ST_MAXSEQ / a small literal — NEVER by a
# runtime dim (D/DFF/E/L locals from ST_DIMS, or m->d/dff/nlayer/nexpert).
echo ""
echo "[no-vla] assert no stack array sized by a runtime dim in student.c"
# match local array decls of the form `type name[EXPR];` and flag any whose
# size expression is a bare runtime dim. The runtime dims in scope are the
# single-letter locals D/E/L and the word DFF (from ST_DIMS), plus m->* .
VLA_HITS="$(grep -nE '\b(float|int|uint8_t|double|char)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*\[(D|E|L|DFF|n|m->d|m->dff|m->nlayer|m->nexpert)\]' \
    "$SRC_NEW" || true)"
if [ -n "$VLA_HITS" ]; then
    echo "  FAIL  [no-vla] a stack array is sized by a runtime dim:"
    echo "$VLA_HITS" | sed 's/^/      /'
    VLA_RC=1
else
    echo "  PASS  [no-vla] no stack array in student.c is sized by D/E/L/DFF/n/m->*"
    echo "        (all scratch bound to ST_*_MAX / KMAX / V / ST_MAXSEQ)"
    VLA_RC=0
fi

# Belt-and-suspenders: the only [VAR] array decls allowed are the fixed bounds.
echo "  scratch-array bounds actually used in student.c:"
grep -oE '\b(float|int|uint8_t)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*\[[A-Za-z_][A-Za-z0-9_]*\]' "$SRC_NEW" \
    | sed -E 's/.*\[([A-Za-z_][A-Za-z0-9_]*)\]/\1/' | sort -u | sed 's/^/      /'

echo ""
if [ "$IDENT_RC" -eq 0 ] && [ "$CERT_RC" -eq 0 ] && [ "$VLA_RC" -eq 0 ]; then
    echo "[result] PASS"; exit 0
else
    echo "[result] FAIL (ident=$IDENT_RC cert=$CERT_RC novla=$VLA_RC)"; exit 1
fi
