#!/bin/bash
# ---------------------------------------------------------------------------
# run_b5_step7.sh - B5 step 7 (research/b5-step7-design.md): step 6's four
# nodes; the trust from the peer's own update (st_merge_trust_prev, the
# receiver keeps what each peer sent the previous round), the merge still
# toward the peer's weights. student_adopt_test.c stages s7unit and
# s7a/s7b/s7y/s7c, judge b5_step7_check.py (R1-R6, NC1, NC2). Not in CI
# (tens of minutes a seed).
#
#   ./run_b5_step7.sh               exit 0 = self-tests green, R1-R6 green, NC1 NC2 red
#   B5S7_MODE=self                  the two self-tests and the gj mutant (seconds)
#   B5S7_SEEDS="131 ... 138"        the judgement seeds (fixed in the design)
#   B5S7_OUT=<dir>  B5S7_JOBS=3
# Builds: plain; gj (-DB5S7_TS_FROM_GJ, step 6's TS, NC1) runs s7a and must
# turn s7unit red; claimed (-DB5S7_PREV_CLAIMED, the near poison claims its
# previous and jumps, NC2) runs s7y.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
OUT="${B5S7_OUT:-$WORK/out}"
SEEDS="${B5S7_SEEDS:-131 132 133 134 135 136 137 138}"
JOBS="${B5S7_JOBS:-3}"
mkdir -p "$OUT"
python3 "$HERE/b5_step7_check.py" --self-test > "$OUT/self.log" 2>&1 \
    || { cat "$OUT/self.log"; echo "[result] FAIL  b5-step7 judge self-test"; exit 1; }
echo "[b5-step7] judge self-test ok ($(grep -c ' ok ' "$OUT/self.log") cases)"
L="$ROOT/arch/common/llm"
SRC="$HERE/student_adopt_test.c $L/held_indep.c $L/cradle.c $L/student.c"
build() {  # build <name> <flags...>
    local n="$1"; shift
    # shellcheck disable=SC2086
    $CC $CFLAGS "$@" $SRC -lm -o "$WORK/$n" || { echo "[build] $n FAILED"; exit 1; }
}
echo "[build] b5-step7: plain, gj, claimed ..."
build plain
build gj -DB5S7_TS_FROM_GJ
build claimed -DB5S7_PREV_CLAIMED
for u in s6unit s7unit; do
    "$WORK/plain" $u > "$OUT/unit-$u.log" 2>&1 \
        || { cat "$OUT/unit-$u.log"; echo "[result] FAIL  b5-step7 $u"; exit 1; }
    echo "[b5-step7] $u ok ($(grep -c '  PASS  ' "$OUT/unit-$u.log") checks)"
done
if "$WORK/gj" s7unit > "$OUT/unit-gj.log" 2>&1; then
    echo "[result] FAIL  b5-step7 the gj build passes s7unit"; exit 1
fi
echo "[b5-step7] the gj build is red ($(grep -c '  FAIL  ' "$OUT/unit-gj.log") checks)"
if [ "${B5S7_MODE:-}" = self ]; then echo "[result] PASS  b5-step7 self-tests"; exit 0; fi
{ for s in $SEEDS; do echo "plain $s s7a plain-$s-a"; echo "plain $s s7b plain-$s-b"; done
  for s in $SEEDS; do echo "plain $s s7y plain-$s-y"; echo "plain $s s7c plain-$s-c"; done
  for s in $SEEDS; do echo "gj $s s7a gj-$s"; echo "claimed $s s7y claimed-$s"; done; } \
    | xargs -P "$JOBS" -L 1 sh -c 'w="$0"; h="$1"; o="$2"; b="$3"; s="$4"; st="$5"; f="$6"; "$w/$b" "$h" "$s" "$st" > "$o/$f.log" 2>&1; echo "[b5-step7] $f rc=$?"' \
        "$WORK" "$HERE/b1_held.txt" "$OUT"
python3 "$HERE/b5_step7_check.py" --seeds "$SEEDS" "$OUT"
