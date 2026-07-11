#!/bin/bash
# ---------------------------------------------------------------------------
# run_depth_compound.sh — host cert for ACCURACY-COMPOUNDING (fable5 Wave-C).
#                         The "verifier-exceeds" minimal loop in miniature.
#
# Sibling of run_depth.sh. run_depth.sh's [depth-compound-verified-only] proves
# distilling V-exact-verified deliberation winners lowers the correct-answer
# LOSS. THIS cert asks whether that win shows up in ONE-SHOT (K=1, greedy, NO
# deliberation) ACCURACY: after distilling the verified winners DLB found by
# SEARCH x VERIFY, does cold one-shot accuracy RISE above the pre-distill one-
# shot accuracy? (The AlphaZero crack: test-time compute amortized into weights.)
#
# It drives the EXACT distill path the LIVE DMN wire calls
# (student_shell.c student_dmn_consolidate -> dlb_compound_distill(g_student,
# ROUNDS, LR, require_verified=1)). Same dlb.c + student.c, host cc, runs
# NATIVELY (NOT under qemu) in seconds.
#
# HARD TEETH (anti-theater): Arm-D (distill UNVERIFIED wrong traces) must NOT
# raise one-shot acc (degrades); the gate-blocked STUB must leave acc EXACTLY
# flat. HONEST-NULL: if the tier=S one-shot gain is below the step threshold it
# is PRINTED as a pre-registered NULL and the cert still PASSES structurally.
#
#   ./run_depth_compound.sh
# Exit 0 = teeth green + structural greps clean (compound claim reported).
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
INC="-I$ROOT/arch/common/llm"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

SRC_TEST="$HERE/depth_compound_test.c"
SRC_DLB="$ROOT/arch/common/llm/dlb.c"
SRC_STU="$ROOT/arch/common/llm/student.c"

echo "[build] ACCURACY-COMPOUND cert (dlb.c + student.c) ... (-Werror=vla, -ffp-contract=off one-math)"
$CC $CFLAGS $INC "$SRC_TEST" "$SRC_DLB" "$SRC_STU" -lm -o "$WORK/depth_compound" \
    || { echo "[build] FAILED"; exit 1; }

# ---- structural [live-wire]: the DMN seam actually calls the distill ----------
# The load-bearing NEW wiring for this wave: student_dmn_consolidate must call
# dlb_compound_distill with the HARD GATE (require_verified=1). If a refactor
# drops the wire this grep goes RED even though the cert (which exercises the
# function directly) stays green — so the LIVE path can't silently rot.
echo ""
echo "[grep] [live-wire] — student_dmn_consolidate calls dlb_compound_distill(...,1)"
SS="$ROOT/arch/common/llm/student_shell.c"
WIRE="$(grep -nE 'dlb_compound_distill\(&g_student,[^;]*,[[:space:]]*1\)' "$SS" || true)"
if [ -n "$WIRE" ]; then
    echo "  PASS  [live-wire] the DMN sleep tick distills verified traces (hard gate on):"
    echo "$WIRE" | sed 's/^/      /'; WIRE_RC=0
else
    echo "  FAIL  [live-wire] student_shell.c does NOT wire dlb_compound_distill(...,require_verified=1)"; WIRE_RC=1
fi

# ---- structural [crown-neutral]: the wire is HOSTED-ONLY ----------------------
# student_shell.c is not built on bare metal (student_stub.o resolves the ABI),
# and dlb.c calls only the public student.h API. Assert the wire lives in the
# hosted TU and touches no bare-metal / crown TU on its distill line.
echo ""
echo "[grep] [crown-neutral] — the live wire is hosted-only (no bare-metal TU on the distill path)"
BAD="$(grep -nE '\b(gl_merge|dtr_|moe_|dmn_[a-z]|r3_|conscience_)[A-Za-z0-9_]*\(' "$SRC_DLB" || true)"
if [ -z "$BAD" ]; then
    echo "  PASS  [crown-neutral] dlb.c uses public student.h API only; wire is in hosted student_shell.c"; CN_RC=0
else
    echo "  FAIL  [crown-neutral] dlb.c reaches a bare-metal / crown TU:"; echo "$BAD"; CN_RC=1
fi

# ---- run the in-process cert (teeth + reported compound claim) ----------------
echo ""
echo "[run] ACCURACY-COMPOUND in-process cert (real dlb loop + real student baby) ..."
"$WORK/depth_compound"
CERT_RC=$?

echo ""
echo "[machine] determinism hash (run on x86_64 AND aarch64; must match):"
"$WORK/depth_compound" --machine | sed 's/^/    /'

echo ""
if [ "$CERT_RC" -eq 0 ] && [ "$WIRE_RC" -eq 0 ] && [ "$CN_RC" -eq 0 ]; then
    echo "[result] PASS"
    echo "[note] Teeth (Arm-D degrades, gate-blocked stub flat) are the load-bearing proof."
    echo "       The one-shot ACCURACY gain is reported (WIN or pre-registered NULL); the"
    echo "       loss-based [depth-compound-verified-only] already proves the distill works."
    exit 0
else
    echo "[result] FAIL (cert=$CERT_RC livewire=$WIRE_RC crownneutral=$CN_RC)"
    exit 1
fi
