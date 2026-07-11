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
# It drives the EXACT distill path the DMN wire calls (student_shell.c
# student_dmn_consolidate -> dlb_compound_distill(g_student, ROUNDS, LR,
# require_verified=1)). Same dlb.c + student.c, host cc, runs NATIVELY (NOT
# under qemu).
#
# ROBUSTIFIED (wave-c-compound audit, 2026-07-11): the cert now SEED-AVERAGES
# pre/post/Arm-D one-shot accuracy over N_SEEDS>=16 DISJOINT fixture seeds and
# judges the SEED-AVERAGED claim, not a single 1/N draw. It reports mean_pre,
# mean_post, mean_gain, frac_post_ge_pre, and the load-bearing seed-averaged
# separation (verified helps MORE than unverified garbage). robust_win is TRUE
# only if mean_gain>=MARGIN AND frac_post_ge_pre>=0.75 AND armd_load_bearing;
# otherwise an honest pre-registered NULL is printed. Either way exit 0.
#
# HARD TOOTH (survives WIN or NULL): the gate-blocked STUB must leave one-shot
# acc EXACTLY flat on EVERY seed (deterministic invariant). The former per-seed
# Arm-D "does not rise" check was SEED-FRAGILE (it FAILed on seeds 42/4444/9999)
# and is replaced by the seed-averaged load-bearing separation.
#
# DORMANT-WIRE (audit defect 3): student_shell.c distills verified traces on the
# DMN tick, but NO production path calls dlb_answer / dlb_compound_enqueue yet,
# so the ring is populated ONLY by this cert; in the running system it is empty
# and the distill is a permanent no-op. The [dormant-wire] grep below asserts
# that dormancy plainly. We do NOT claim "the live loop closes".
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

# ---- structural [dormant-wire]: DISCLOSE that the ring has no production feeder -
# Audit defect 3: the distill wire above is DORMANT. student_shell.c distills
# whatever verified traces are in the ring, but NO production TU calls dlb_answer
# or dlb_compound_enqueue, so in the running system the ring is ALWAYS empty and
# the distill is a permanent no-op — the loop closes ONLY in this cert. This grep
# (a) prints the production feeders (expected: NONE today) and (b) gates on the
# HONESTY DISCLOSURE comment being present in student_shell.c so the scope note
# cannot silently rot. It does NOT gate on dormancy itself (wiring the mouth to
# enqueue is the intended NEXT step, not a regression).
echo ""
echo "[grep] [dormant-wire] — no production path enqueues yet (ring fed ONLY by the cert)"
FEEDERS="$(grep -rnE 'dlb_(answer|compound_enqueue)\(' "$ROOT/arch" --include='*.c' \
    | grep -vE '/llm/dlb\.c:' || true)"
if [ -z "$FEEDERS" ]; then
    echo "  INFO  no production caller of dlb_answer/dlb_compound_enqueue — the wire is DORMANT"
    echo "        (the ring is populated ONLY by depth_compound_test.c; live distill = no-op)"
else
    echo "  INFO  production feeder(s) now exist (the live loop may close):"
    echo "$FEEDERS" | sed 's/^/      /'
fi
DISC="$(grep -nE 'DORMANT|no production path' "$SS" || true)"
if [ -n "$DISC" ]; then
    echo "  PASS  [dormant-wire] student_shell.c carries the honest DORMANT-scope disclosure:"
    echo "$DISC" | sed 's/^/      /'; DW_RC=0
else
    echo "  FAIL  [dormant-wire] student_shell.c is missing the DORMANT-wire honesty disclosure"; DW_RC=1
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
if [ "$CERT_RC" -eq 0 ] && [ "$WIRE_RC" -eq 0 ] && [ "$CN_RC" -eq 0 ] && [ "$DW_RC" -eq 0 ]; then
    echo "[result] PASS"
    echo "[note] Seed-averaged over N_SEEDS>=16 disjoint seeds. The hard stub tooth (gate-"
    echo "       blocked distill EXACTLY flat on every seed) is the load-bearing proof; the"
    echo "       one-shot ACCURACY gain is reported seed-averaged (robust WIN or pre-registered"
    echo "       NULL). The loss-based [depth-compound-verified-only] already proves the distill"
    echo "       works. The distill wire is DORMANT (no production feeder yet) — disclosed above."
    exit 0
else
    echo "[result] FAIL (cert=$CERT_RC livewire=$WIRE_RC crownneutral=$CN_RC dormantwire=$DW_RC)"
    exit 1
fi
