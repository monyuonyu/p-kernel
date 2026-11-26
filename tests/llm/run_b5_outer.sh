#!/bin/bash
# ---------------------------------------------------------------------------
# run_b5_outer.sh - B5 step 11 (research/b5-outer.md): the round's merge as an
# outer gradient with Nesterov momentum (DiLoCo, arXiv:2311.08105 Alg. 1),
# st_round_outer. student_outer_test.c (unit = O4, arms per seed), judge
# b5_outer_check.py (O1-O3, NC1, NC2). Written before the function: on a tree
# without st_round_outer the build stops at the test's #error = RED.
# Not in CI (minutes a seed); the unit (seconds) goes to CI with the function.
#
#   ./run_b5_outer.sh                exit 0 = unit green, O1-O3 green, NC1 NC2 red
#                                    (the product rule P is printed, it is not the exit code)
#   B5O_MODE=self                    the unit and the two NC units only (seconds)
#   B5O_SEEDS="311 ... 318"          the judgement seeds (fixed in the design)
#   B5O_OUT=<dir>  B5O_JOBS=4
# Builds (EVERY 1 = a round every pass, 32 outer steps): plain; ncplain
# (-DB5O_NC_PLAIN, the function forced to lr 1 mu 0 = the plain merge) and
# ncflip (-DB5O_NC_FLIP, the outer gradient's sign flipped) must turn the
# unit red and O1 red. plain2, plain4 (EVERY 2, 4) run trim4 and outer4 for P.
# About 2 hours with 4 jobs on the 15 W laptop: run it detached.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
OUT="${B5O_OUT:-$WORK/out}"
SEEDS="${B5O_SEEDS:-311 312 313 314 315 316 317 318}"
JOBS="${B5O_JOBS:-4}"
mkdir -p "$OUT"
python3 "$HERE/b5_outer_check.py" --self-test > "$OUT/self.log" 2>&1 \
    || { cat "$OUT/self.log"; echo "[result] FAIL  b5-outer judge self-test"; exit 1; }
echo "[b5-outer] judge self-test ok ($(grep -c ' ok ' "$OUT/self.log") cases)"
L="$ROOT/arch/common/llm"
SRC="$HERE/student_outer_test.c $L/held_indep.c $L/cradle.c $L/student.c"
build() {  # build <name> <flags...>
    local n="$1"; shift
    # shellcheck disable=SC2086
    if ! $CC $CFLAGS "$@" $SRC -lm -o "$WORK/$n" > "$OUT/cc-$n.log" 2>&1; then
        grep -m1 -E 'error' "$OUT/cc-$n.log"
        echo "[result] FAIL  b5-outer build $n (st_round_outer missing?)"; exit 1
    fi
}
echo "[build] b5-outer: plain, ncplain (-DB5O_NC_PLAIN), ncflip (-DB5O_NC_FLIP) at EVERY 1; plain2, plain4 ..."
build plain -DEVERY=1
build ncplain -DEVERY=1 -DB5O_NC_PLAIN
build ncflip -DEVERY=1 -DB5O_NC_FLIP
build plain2 -DEVERY=2
build plain4 -DEVERY=4
"$WORK/plain" unit > "$OUT/unit.log" 2>&1 \
    || { cat "$OUT/unit.log"; echo "[result] FAIL  b5-outer st_round_outer unit (O4)"; exit 1; }
echo "[b5-outer] unit ok ($(grep -c '  PASS  ' "$OUT/unit.log") checks)"
for b in ncplain ncflip; do
    if "$WORK/$b" unit > "$OUT/unit-$b.log" 2>&1; then
        echo "[result] FAIL  b5-outer the $b build passes the unit"; exit 1
    fi
    echo "[b5-outer] the $b build is red in the unit ($(grep -c '  FAIL  ' "$OUT/unit-$b.log") checks)"
done
if [ "${B5O_MODE:-}" = self ]; then echo "[result] PASS  b5-outer self-tests"; exit 0; fi
{ for s in $SEEDS; do echo "plain $s all"; done
  for s in $SEEDS; do echo "ncplain $s nc"; echo "ncflip $s nc"; echo "plain2 $s nc"; echo "plain4 $s nc"; done; } \
    | xargs -P "$JOBS" -L 1 sh -c 'w="$0"; h="$1"; o="$2"; b="$3"; s="$4"; st="$5"; "$w/$b" "$h" "$s" "$st" > "$o/$b-$s.log" 2>&1; echo "[b5-outer] $b seed $s rc=$?"' \
        "$WORK" "$HERE/b1_held.txt" "$OUT"
python3 "$HERE/b5_outer_check.py" --seeds "$SEEDS" "$OUT"
