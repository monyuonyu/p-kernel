#!/bin/bash
# ---------------------------------------------------------------------------
# run_b5_step9.sh - B5 step 9 (research/b5-step9-design.md): step 8's arms
# (st_merge_trust_echo unchanged), the ruler fixed: R7' compares near2 (the
# median of 3 poison noise seeds, rep 0 1 2) with pair2 (the two poisons sent
# to nobody). student_adopt_test.c stages s9a s9b s9y s9c s9d s9p s9r1 s9r2,
# the NC1 build (-DB5S8_NO_ECHO) stage s9n; judge b5_step9_check.py. Not in CI
# (about two hours on 4 cores).
#
#   ./run_b5_step9.sh               exit 0 = self-tests green, R1-R9 (R7') green, NC1 red
#   B5S9_MODE=self                  the judge's self-test and the units (seconds)
#   B5S9_SEEDS="149 ... 156"        the judgement seeds (fixed in the design)
#   B5S9_OUT=<dir>  B5S9_JOBS=3
# The jobs R7' needs (p b r1 r2 and NC1) are started first, seed by seed.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
OUT="${B5S9_OUT:-$WORK/out}"
SEEDS="${B5S9_SEEDS:-149 150 151 152 153 154 155 156}"
JOBS="${B5S9_JOBS:-3}"
mkdir -p "$OUT"
python3 "$HERE/b5_step9_check.py" --self-test > "$OUT/self.log" 2>&1 \
    || { cat "$OUT/self.log"; echo "[result] FAIL  b5-step9 judge self-test"; exit 1; }
echo "[b5-step9] judge self-test ok ($(grep -c ' ok ' "$OUT/self.log") cases)"
L="$ROOT/arch/common/llm"
SRC="$HERE/student_adopt_test.c $L/held_indep.c $L/cradle.c $L/student.c"
build() {  # build <name> <flags...>
    local n="$1"; shift
    # shellcheck disable=SC2086
    $CC $CFLAGS "$@" $SRC -lm -o "$WORK/$n" || { echo "[build] $n FAILED"; exit 1; }
}
echo "[build] b5-step9: plain, noecho ..."
build plain
build noecho -DB5S8_NO_ECHO
for u in s6unit s7unit s8unit; do
    "$WORK/plain" $u > "$OUT/unit-$u.log" 2>&1 \
        || { cat "$OUT/unit-$u.log"; echo "[result] FAIL  b5-step9 $u"; exit 1; }
    echo "[b5-step9] $u ok ($(grep -c '  PASS  ' "$OUT/unit-$u.log") checks)"
done
if "$WORK/noecho" s8unit > "$OUT/unit-noecho.log" 2>&1; then
    echo "[result] FAIL  b5-step9 the noecho build passes s8unit"; exit 1
fi
echo "[b5-step9] the noecho build is red ($(grep -c '  FAIL  ' "$OUT/unit-noecho.log") checks)"
if [ "${B5S9_MODE:-}" = self ]; then echo "[result] PASS  b5-step9 self-tests"; exit 0; fi
{ for s in $SEEDS; do for p in p b r1 r2; do echo "plain $s s9$p plain-$s-$p"; done; echo "noecho $s s9n noecho-$s"; done
  for s in $SEEDS; do for p in a y c d; do echo "plain $s s9$p plain-$s-$p"; done; done; } \
    | xargs -P "$JOBS" -L 1 sh -c 'w="$0"; h="$1"; o="$2"; b="$3"; s="$4"; st="$5"; f="$6"; "$w/$b" "$h" "$s" "$st" > "$o/$f.log" 2>&1; echo "[b5-step9] $f rc=$?"' \
        "$WORK" "$HERE/b1_held.txt" "$OUT"
python3 "$HERE/b5_step9_check.py" --seeds "$SEEDS" "$OUT"
