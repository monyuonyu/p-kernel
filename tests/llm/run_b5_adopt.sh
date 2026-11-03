#!/bin/bash
# ---------------------------------------------------------------------------
# run_b5_adopt.sh - [b5-adopt] B5 step 2 (research/b5-step2.md): the round's
# average goes into a copy, st_round_adopt takes it only if it is finite and
# its held loss is lower. tests/llm/student_adopt_test.c, judge b5_adopt_check.py.
#
#   ./run_b5_adopt.sh            exit 0 = H1-H4 green and both NCs red
#   B5A_MODE=unit                the gate's unit cases only (seconds)
#   B5A_SEEDS="91 ... 98"        the judgement seeds (fixed in the design)
#   B5A_OUT=<dir>  B5A_JOBS=3
# NCs: -DST_ADOPT_ALWAYS (the gate takes every copy) must turn H2 red,
#      -DST_ADOPT_INVERT (the comparison the other way) must turn H1 red.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
OUT="${B5A_OUT:-$WORK/out}"
SEEDS="${B5A_SEEDS:-91 92 93 94 95 96 97 98}"
JOBS="${B5A_JOBS:-3}"
mkdir -p "$OUT"
L="$ROOT/arch/common/llm"
SRC="$HERE/student_adopt_test.c $L/held_indep.c $L/cradle.c $L/student.c"

build() {  # build <name> <flags...>
    local n="$1"; shift
    # shellcheck disable=SC2086
    $CC $CFLAGS "$@" $SRC -lm -o "$WORK/$n" || { echo "[build] $n FAILED"; exit 1; }
}
echo "[build] b5-adopt: plain, always (-DST_ADOPT_ALWAYS), invert (-DST_ADOPT_INVERT) ..."
build plain
"$WORK/plain" unit | tee "$OUT/plain-unit.log"
if [ "${B5A_MODE:-}" = unit ]; then
    grep -q '^\[b5a\] unit PASS' "$OUT/plain-unit.log" && { echo "[result] PASS  b5-adopt unit"; exit 0; }
    echo "[result] FAIL  b5-adopt unit"; exit 1
fi
build always -DST_ADOPT_ALWAYS
build invert -DST_ADOPT_INVERT
for s in $SEEDS; do for b in plain always invert; do echo "$b $s"; done; done \
    | xargs -P "$JOBS" -L 1 sh -c 'w="$0"; h="$1"; o="$2"; b="$3"; s="$4"; "$w/$b" "$h" "$s" > "$o/$b-$s.log" 2>&1; echo "[b5-adopt] $b seed $s rc=$?"' \
        "$WORK" "$HERE/b1_held.txt" "$OUT"
python3 "$HERE/b5_adopt_check.py" --seeds "$SEEDS" "$OUT"
