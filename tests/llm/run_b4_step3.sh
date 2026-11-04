#!/bin/bash
# ---------------------------------------------------------------------------
# run_b4_step3.sh - [b4-step3] B4 stage 3 (research/b4-step3.md): early stop
# on the fixture's held side, with Adam eps 1e-8 and rate 1e-3 (e3), against
# stage 1 (s1: eps 1/64, 3e-3). Lines fixed in the design before any run
# (judge: b4_step3_check.py):
#   H1  e3's chosen point (lowest "stop" bpb, earliest on ties) has b1_held
#       bpb <= s1's chosen point in every pair, and <= s1 - 0.05 in 6 of 8
#   H2  e3 at k=1536 <= s1 at k=1536 in every pair, and - 0.05 in 6 of 8
#   H3  e3's chosen point - the lowest b1_held of its 16 points <= 0.05, 8/8
#   NC1 nc1 (e3 on the old root, -DST_SQRT_OLD) in place of e3 turns H1 red
#   NC2 e3 choosing on its training windows ("trainwin") in place of the
#       held side: expected to turn H3 red; a reading, not a gate
#   r5  (5e-4, 1e-8) is a reading only and does not change the verdict
# One binary per arm, all built with -DB4_EVERY96 (a point every 96 steps,
# 16 points up to 1536). NC2 is read from the e3 logs (choosing on another
# column does not change the training). The four builds of one (tier, seed)
# run side by side (this machine throttles). D = 3072.
#
#   ./run_b4_step3.sh          exit 0 = H1 H2 H3 green and NC1 red (not in CI)
#   B4S_OUT=<dir>              keep the per-arm logs there (default: temp)
#   B4S_SEEDS="73 74 75 76"  B4S_TIERS="0 1"   (other seeds are a reading)
#   B4S_NICE=19                run the arms under nice (default 19, so a CI
#                              job on this machine is not starved)
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla -DB4_EVERY96"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
OUT="${B4S_OUT:-$WORK/out}"
SEEDS="${B4S_SEEDS:-73 74 75 76}"
TIERS="${B4S_TIERS:-0 1}"
NICE="${B4S_NICE:-19}"
mkdir -p "$OUT"
L="$ROOT/arch/common/llm"
SRC="$HERE/b2_curve_test.c $L/held_indep.c $L/cradle.c $L/student.c"

build() {  # build <name> <flags...>
    local n="$1"; shift
    # shellcheck disable=SC2086
    $CC $CFLAGS "$@" $SRC -lm -o "$WORK/$n" || { echo "[build] $n FAILED"; exit 1; }
}
echo "[build] b4-step3: s1, e3, r5, nc1 (all -DB4_EVERY96) ..."
build s1  -DST_LR=3e-3f -DST_ADAM_EPS=0.015625f
build e3  -DST_LR=1e-3f -DST_ADAM_EPS=1e-8f
build r5  -DST_LR=5e-4f -DST_ADAM_EPS=1e-8f
build nc1 -DST_LR=1e-3f -DST_ADAM_EPS=1e-8f -DST_SQRT_OLD

for t in $TIERS; do
    for s in $SEEDS; do
        for b in s1 e3 r5 nc1; do
            nice -n "$NICE" "$WORK/$b" "$HERE/b1_held.txt" "$t" 3072 "$s" 1536 > "$OUT/$b-$t-$s.log" 2>&1 &
        done
        wait
        echo "[b4-step3] tier $t seed $s done $(date -Is)"
    done
done
python3 "$HERE/b4_step3_check.py" --tiers "$TIERS" --seeds "$SEEDS" "$OUT"
