#!/bin/bash
# ---------------------------------------------------------------------------
# run_depth.sh — host cert for DEPTH / test-time DELIBERATION (DLB).
#                depth_iq_path_design.md §6. The load-bearing falsifier.
#
# Builds the REAL dlb.c loop (search x verify) + the REAL compounding ring over
# the REAL student.c byte-baby and runs the [depth-*] arms on a MINIMAL
# deterministic V-EXACT micro-fixture (mod-10 arithmetic composition). This is a
# HOST cc build that runs NATIVELY (NOT under qemu) in seconds — so the
# anti-theater STUB proof (STUB-SEARCH K=1 RED, STUB-VERIFY random RED) runs in
# the sandbox WITHOUT the full training run. The heavy [depth-deliberation-gain]
# general-domain / [depth-teacher-approach] training legs are deferred to the
# ThinkPad self-hosted runner (see the ci.yml block); deferring them is CORRECT,
# not a gap (design §6.2 pre-registered NULL, §8 toy-scale).
#
# dlb.c calls ONLY the public student.h API (crown-neutral, hosted-only), so the
# build needs nothing but the LLM dir on the include path.
#
#   ./run_depth.sh
# Exit 0 = all load-bearing [depth-*] gates green + structural greps clean.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
INC="-I$ROOT/arch/common/llm"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

SRC_TEST="$HERE/depth_test.c"
SRC_DLB="$ROOT/arch/common/llm/dlb.c"
SRC_STU="$ROOT/arch/common/llm/student.c"

echo "[build] DEPTH cert (dlb.c + student.c) ... (-Werror=vla, -ffp-contract=off one-math)"
$CC $CFLAGS $INC "$SRC_TEST" "$SRC_DLB" "$SRC_STU" -lm -o "$WORK/depth" \
    || { echo "[build] FAILED"; exit 1; }

# ---- structural [no-vla] over dlb.c (crown discipline: no runtime-sized stack) -
echo ""
echo "[no-vla] assert no stack array in dlb.c sized by a runtime dim"
VLA_HITS="$(grep -nE '\b(float|int|uint8_t|double|char)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*\[(K|qn|cn|dn|an|n|rounds|k_used)\]' \
    "$SRC_DLB" || true)"
if [ -n "$VLA_HITS" ]; then
    echo "  FAIL  [no-vla] a stack array is sized by a runtime dim:"; echo "$VLA_HITS" | sed 's/^/      /'; VLA_RC=1
else
    echo "  PASS  [no-vla] dlb.c scratch is bound to fixed ST_GEN_CAP / DLB_TRACE_MAX (heap ring)"; VLA_RC=0
fi

# ---- structural [crown-neutral]: dlb.c calls ONLY public student.h API --------
# It must NOT reach into student.c internals, moe.c, dmn.c, or the R3 crown. Match
# actual CALL syntax (the identifier immediately followed by '(', or the rw[]
# index) so PROSE in a comment that merely NAMES a bare-metal TU cannot false-
# positive — only a real call/index is a violation.
echo ""
echo "[grep] [crown-neutral] — dlb.c touches no bare-metal TU (public student.h only)"
BAD="$(grep -nE '\b(gl_merge|dtr_|moe_|dmn_|r3_|conscience_)[A-Za-z0-9_]*\(|[^A-Za-z0-9_]rw\[|#include "student\.c"' "$SRC_DLB" || true)"
if [ -z "$BAD" ]; then
    echo "  PASS  [crown-neutral] dlb.c uses public student.h API only (hosted; bare-metal .text untouched)"; CN_RC=0
else
    echo "  FAIL  [crown-neutral] dlb.c reaches a bare-metal / crown TU:"; echo "$BAD"; CN_RC=1
fi

# ---- structural [hard-gate]: production distill refuses unverified traces ------
echo ""
echo "[grep] [hard-gate] — dlb_compound_distill has the require_verified skip (§3.4 gate)"
GATE="$(grep -nE 'require_verified && !t->verified' "$SRC_DLB" || true)"
if [ -n "$GATE" ]; then
    echo "  PASS  [hard-gate] the HARD GATE (verified-only distill) is present in dlb.c"; HG_RC=0
else
    echo "  FAIL  [hard-gate] the verified-only skip is missing — the learner trap is unguarded"; HG_RC=1
fi

# ---- run the in-process cert (the load-bearing arms) --------------------------
echo ""
echo "[run] DEPTH in-process cert (real dlb loop + real student baby) ..."
"$WORK/depth"
CERT_RC=$?

echo ""
echo "[machine] per-case byte hashes (cross-arch determinism diff — run on x86_64 AND aarch64):"
"$WORK/depth" --machine | sed 's/^/    /'

echo ""
if [ "$CERT_RC" -eq 0 ] && [ "$VLA_RC" -eq 0 ] && [ "$CN_RC" -eq 0 ] && [ "$HG_RC" -eq 0 ]; then
    echo "[result] PASS"
    echo "[note] The two teeth (STUB-SEARCH K=1 RED, STUB-VERIFY random RED) prove the"
    echo "       search x verify decomposition is load-bearing WITHOUT the full training run."
    echo "       General-domain / model-reasoning gain is a PRE-REGISTERED NULL at tier=S."
    exit 0
else
    echo "[result] FAIL (cert=$CERT_RC novla=$VLA_RC crownneutral=$CN_RC hardgate=$HG_RC)"
    exit 1
fi
