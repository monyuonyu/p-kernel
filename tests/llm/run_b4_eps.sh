#!/bin/bash
# ---------------------------------------------------------------------------
# run_b4_eps.sh - [b4-eps] stage 2 of research/b4-numerics.md: Adam's eps 1e-8
# and the rate 1e-3 (s2) against stage 1 (s1: eps 1/64, 3e-3), on the
# held text (tests/llm/b1_held.txt) with b2_curve_test.c.
# Lines fixed in the design before any run (judge: b4_eps_check.py):
#   G1  in all 4 arms (S and M x seeds 71 and 72) the best bpb of s2
#       is <= s1's best - 0.05
#   G2  in all 4 arms s2's bpb at k=1536 is <= its own best + 0.1
#   NC  s2 with the old root (-DST_SQRT_OLD) turns G1 red
# Both arms name their eps and rate, so the cert does not depend on the
# defaults. First run 2026-11-03 (d86154aa): RED, G1 on M seed 72 (-0.003)
# and G2 on S seed 72 (+0.108); the defaults stay stage 1.
# D = 3072, k up to 1536. The three builds of one (tier, seed) run side by
# side (this machine throttles; comparisons across time are not valid).
#
#   ./run_b4_eps.sh              exit 0 = G1, G2 green and NC red (not in CI)
#   B4E_OUT=<dir>                keep the per-arm logs there (default: temp)
#   B4E_SEEDS="71 72"  B4E_TIERS="0 1"   (other seeds are a reading, not the cert)
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
OUT="${B4E_OUT:-$WORK/out}"
SEEDS="${B4E_SEEDS:-71 72}"
TIERS="${B4E_TIERS:-0 1}"
mkdir -p "$OUT"
L="$ROOT/arch/common/llm"
SRC="$HERE/b2_curve_test.c $L/held_indep.c $L/cradle.c $L/student.c"

build() {  # build <name> <flags...>
    local n="$1"; shift
    # shellcheck disable=SC2086
    $CC $CFLAGS "$@" $SRC -lm -o "$WORK/$n" || { echo "[build] $n FAILED"; exit 1; }
}
echo "[build] b4-eps: s1 (stage 1), s2 (stage 2), nc (stage 2 on the old root) ..."
build s1 -DST_LR=3e-3f -DST_ADAM_EPS=0.015625f
build s2 -DST_LR=1e-3f -DST_ADAM_EPS=1e-8f
build nc -DST_LR=1e-3f -DST_ADAM_EPS=1e-8f -DST_SQRT_OLD

for t in $TIERS; do
    for s in $SEEDS; do
        for b in s1 s2 nc; do
            "$WORK/$b" "$HERE/b1_held.txt" "$t" 3072 "$s" 1536 > "$OUT/$b-$t-$s.log" 2>&1 &
        done
        wait
        echo "[b4-eps] tier $t seed $s done $(date -Is)"
    done
done
python3 "$HERE/b4_eps_check.py" --tiers "$TIERS" --seeds "$SEEDS" "$OUT"
