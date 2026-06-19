#!/bin/bash
# ---------------------------------------------------------------------------
# run_kv.sh — host cert for the KV cache in the Cradle baby's generation path
#             (arch/common/llm/student.c, wave-kv-cache).
#
# Certs (all self-contained, no network, no teacher file):
#   [kv-equivalence]  cached-gen vs recompute-gen produce BYTE-IDENTICAL output
#                     (same sampled bytes + same FNV-1a over every step's logit
#                     row) across several prompts, sampling configs, all three
#                     tiers (S/M/L), and a prompt LONGER than ST_MAXSEQ (the
#                     window-slide / cache-reprime path).
#   [kv-speedup]      tokens/sec cached vs recompute on the M tier, real numbers
#                     reported; gate = cached at least as fast (floor, no flake).
#   [no-vla]          grep tripwire: NO stack array in student.c is sized by a
#                     runtime dim (m->d/dff/nlayer/nexpert or the ST_DIMS locals
#                     D/E/L/DFF or n) — every scratch array is bound to a fixed
#                     ST_*_MAX / KMAX / V / ST_MAXSEQ.
#
#   ./run_kv.sh
# Exit 0 = all certs PASS + the no-vla grep is clean.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

SRC_TEST="$HERE/student_kv_test.c"
SRC_STU="$ROOT/arch/common/llm/student.c"

echo "[build] kv cert (-O1 -ffp-contract=off) ..."
$CC $CFLAGS "$SRC_TEST" "$SRC_STU" -o "$WORK/kv" \
    || { echo "[build] FAILED"; exit 1; }

# ---- [no-vla]: grep tripwire over student.c ----
# Every stack-local array in student.c must be sized by a FIXED bound: ST_*_MAX
# / KMAX / V / ST_VOCAB / ST_MAXSEQ — NEVER by a runtime dim (D/E/L/DFF locals
# from ST_DIMS, n, or m->d/dff/nlayer/nexpert).
echo ""
echo "[no-vla] assert no stack array sized by a runtime dim in student.c"
VLA_HITS="$(grep -nE '\b(float|int|uint8_t|double|char)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*\[(D|E|L|DFF|n|m->d|m->dff|m->nlayer|m->nexpert)\]' \
    "$SRC_STU" || true)"
if [ -n "$VLA_HITS" ]; then
    echo "  FAIL  [no-vla] a stack array is sized by a runtime dim:"
    echo "$VLA_HITS" | sed 's/^/      /'
    VLA_RC=1
else
    echo "  PASS  [no-vla] no stack array in student.c is sized by D/E/L/DFF/n/m->*"
    echo "        (all scratch bound to ST_*_MAX / KMAX / V / ST_MAXSEQ)"
    VLA_RC=0
fi
echo "  scratch-array bounds actually used in student.c:"
grep -oE '\b(float|int|uint8_t)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*\[[A-Za-z_][A-Za-z0-9_]*\]' "$SRC_STU" \
    | sed -E 's/.*\[([A-Za-z_][A-Za-z0-9_]*)\]/\1/' | sort -u | sed 's/^/      /'

# ---- run the equivalence + speedup cert ----
echo ""
echo "[run] kv equivalence + speedup cert ..."
"$WORK/kv"
CERT_RC=$?

# ---- machine view: print the EQ_HASH lines (the byte-identical proof) ----
echo ""
echo "[machine] per-case logit-hash match (recompute vs cached):"
"$WORK/kv" --machine | sed 's/^/    /'

echo ""
if [ "$CERT_RC" -eq 0 ] && [ "$VLA_RC" -eq 0 ]; then
    echo "[result] PASS"; exit 0
else
    echo "[result] FAIL (cert=$CERT_RC novla=$VLA_RC)"; exit 1
fi
