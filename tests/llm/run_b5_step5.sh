#!/bin/bash
# ---------------------------------------------------------------------------
# run_b5_step5.sh - B5 step 5 (research/b5-step5-design.md): four nodes, each a
# quarter, one or two send garbage; the coordinate-wise trimmed mean
# (st_merge_trimmed, t = 1) against the mean, on step 4's form.
# student_adopt_test.c stages s5unit and s5, judge b5_step5_check.py
# (T1-T4, NC1, NC2). Not in CI (about 20 min a seed for the plain build).
#
#   ./run_b5_step5.sh               exit 0 = self-tests green, T1-T4 green, NC1 NC2 red
#   B5S5_MODE=self                  the two self-tests only (seconds)
#   B5S5_SEEDS="115 ... 122"        the judgement seeds (fixed in the design)
#   B5S5_OUT=<dir>  B5S5_JOBS=3
# Builds: plain; trimoff (-DB5S5_TRIM_OFF, the same function averages every
# value) runs the two arms of T1; nogate (-DB5S5_NO_GATE, every candidate
# replaces live) runs the two arms of T4. top2 (-DB5S5_MUT_TOP2) must turn the
# function's self-test red.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
OUT="${B5S5_OUT:-$WORK/out}"
SEEDS="${B5S5_SEEDS:-115 116 117 118 119 120 121 122}"
JOBS="${B5S5_JOBS:-3}"
mkdir -p "$OUT"
python3 "$HERE/b5_step5_check.py" --self-test > "$OUT/self.log" 2>&1 \
    || { cat "$OUT/self.log"; echo "[result] FAIL  b5-step5 judge self-test"; exit 1; }
echo "[b5-step5] judge self-test ok ($(grep -c ' ok ' "$OUT/self.log") cases)"
L="$ROOT/arch/common/llm"
SRC="$HERE/student_adopt_test.c $L/held_indep.c $L/cradle.c $L/student.c"
build() {  # build <name> <flags...>
    local n="$1"; shift
    # shellcheck disable=SC2086
    $CC $CFLAGS "$@" $SRC -lm -o "$WORK/$n" || { echo "[build] $n FAILED"; exit 1; }
}
echo "[build] b5-step5: plain, top2 (-DB5S5_MUT_TOP2) ..."
build plain
build top2 -DB5S5_MUT_TOP2
"$WORK/plain" s5unit > "$OUT/unit.log" 2>&1 \
    || { cat "$OUT/unit.log"; echo "[result] FAIL  b5-step5 st_merge_trimmed self-test"; exit 1; }
echo "[b5-step5] st_merge_trimmed self-test ok ($(grep -c '  PASS  ' "$OUT/unit.log") checks)"
if "$WORK/top2" s5unit > "$OUT/unit-top2.log" 2>&1; then
    echo "[result] FAIL  b5-step5 the top-two mutant passes the self-test"; exit 1
fi
echo "[b5-step5] the top-two mutant is red ($(grep -c '  FAIL  ' "$OUT/unit-top2.log") checks)"
if [ "${B5S5_MODE:-}" = self ]; then echo "[result] PASS  b5-step5 self-tests"; exit 0; fi
echo "[build] b5-step5: trimoff (-DB5S5_TRIM_OFF), nogate (-DB5S5_NO_GATE) ..."
build trimoff -DB5S5_TRIM_OFF
build nogate -DB5S5_NO_GATE
# the long plain runs first, then the short NC ones
{ for s in $SEEDS; do echo "plain $s s5"; done
  for s in $SEEDS; do echo "trimoff $s s5t1"; echo "nogate $s s5t4"; done; } \
    | xargs -P "$JOBS" -L 1 sh -c 'w="$0"; h="$1"; o="$2"; b="$3"; s="$4"; st="$5"; "$w/$b" "$h" "$s" "$st" > "$o/$b-$s.log" 2>&1; echo "[b5-step5] $b seed $s rc=$?"' \
        "$WORK" "$HERE/b1_held.txt" "$OUT"
python3 "$HERE/b5_step5_check.py" --seeds "$SEEDS" "$OUT"
