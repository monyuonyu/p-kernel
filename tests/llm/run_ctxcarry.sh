#!/bin/bash
# ---------------------------------------------------------------------------
# run_ctxcarry.sh — host cert for SCALE-WALL rung C1 [ctx-carry]
#                   (scale_wall_design.md §8; arch/common/llm/student.c v2).
#
# C1 = RoPE + ST_MAXSEQ 64->256 in the Cradle baby: a fact told >=96 bytes back
# can be USED in the completion (context carry — the atomic unit of conversation).
#
# Certs (all self-contained; NO gguf file, NO network):
#   [ctx-carry]             A(d)=CE(ans|ablated)-CE(ans|intact) curve, full 256
#                           window, swept over d — PRINTED (the walk-to-the-wall;
#                           the magnitude is the measured deliverable, NOT gated).
#   [ctx-carry-clamp]       LOAD-BEARING FALSIFIER: re-eval the TRAINED model with
#                           the window clamped to 64 -> A(d>=96) COLLAPSES to ~0.
#   [ctx-carry-window]      ANTI-THEATER (deterministic): a distant fact byte
#                           shifts the answer logits with the full window but is
#                           provably invisible under the 64-clamp (RED-when-stubbed).
#   [gen-cohort-island]     a v1 blob offered to a v2 model is REFUSED everywhere,
#                           model byte-unchanged (NS_STUDENT_VER 1->2 load-bearing).
#   [ctx-carry-nope]        RoPE-vs-NoPE MEASURED side-by-side — PRINTED, NOT gated.
#   [ctx-carry-determinism] fixed-probe-stream FNV (cross-arch pin).
#   [ctx-carry-exclusion]   recall-lookup exclusion (Q marker + R3 OOV).
#
# Modes: default = CI (reduced sweep, bounded runtime for strict CI).
#        CTXCARRY_FULL=1 (or arg "full") = the full nightly curve (minutes).
#
#   ./run_ctxcarry.sh              # reduced sweep, exit 0 = gated arms PASS
#   CTXCARRY_FULL=1 ./run_ctxcarry.sh   # full curve (nightly)
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

SRC_TEST="$HERE/ctxcarry_test.c"
SRC_STU="$ROOT/arch/common/llm/student.c"

MODE="ci"
if [ "${CTXCARRY_FULL:-0}" = "1" ] || [ "${1:-}" = "full" ]; then MODE="full"; fi

echo "[build] ctx-carry cert (-O1 -ffp-contract=off) ..."
$CC $CFLAGS "$SRC_TEST" "$SRC_STU" -o "$WORK/ctxcarry" \
    || { echo "[build] FAILED"; exit 1; }

# ---- [no-vla] tripwire over student.c (the widened ST_MAXSEQ must not have
# introduced any stack array sized by a runtime dim). ----
echo ""
echo "[no-vla] assert no stack array in student.c is sized by a runtime dim"
VLA_HITS="$(grep -nE '\b(float|int|uint8_t|double|char)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*\[(D|E|L|DFF|n|m->d|m->dff|m->nlayer|m->nexpert)\]' \
    "$SRC_STU" || true)"
if [ -n "$VLA_HITS" ]; then
    echo "  FAIL  [no-vla] a stack array is sized by a runtime dim:"
    echo "$VLA_HITS" | sed 's/^/      /'
    VLA_RC=1
else
    echo "  PASS  [no-vla] all scratch bound to ST_*_MAX / KMAX / V / ST_MAXSEQ"
    VLA_RC=0
fi

echo ""
echo "[run] ctx-carry cert (mode=$MODE) ..."
"$WORK/ctxcarry" "$MODE"
CERT_RC=$?

echo ""
if [ "$CERT_RC" -eq 0 ] && [ "$VLA_RC" -eq 0 ]; then
    echo "[result] PASS"; exit 0
else
    echo "[result] FAIL (cert=$CERT_RC novla=$VLA_RC)"; exit 1
fi
