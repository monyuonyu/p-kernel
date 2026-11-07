#!/bin/bash
# ---------------------------------------------------------------------------
# run_b5_step6.sh - B5 step 6 (research/b5-step6-design.md): four nodes, each a
# quarter, one or two poisoned; own-anchored trust (st_merge_trust, the form of
# FLTrust) against the trimmed mean of step 5 and one node alone.
# student_adopt_test.c stages s6unit and s6a/s6b/s6c/s6r1, judge
# b5_step6_check.py (R1-R5, NC1, NC2). Not in CI (tens of minutes a seed).
#
#   ./run_b5_step6.sh               exit 0 = self-tests green, R1-R5 green, NC1 NC2 red
#   B5S6_MODE=self                  the two self-tests and the three mutant builds (seconds)
#   B5S6_SEEDS="123 ... 130"        the judgement seeds (fixed in the design)
#   B5S6_OUT=<dir>  B5S6_JOBS=3
# Builds: plain; flat (-DB5S6_TRUST_FLAT, every TS 1, NC1) and nonorm
# (-DB5S6_NO_NORM, no scaling to |g0|, NC2) run trust4-garb2 only. Those two
# and self (-DB5S6_MUT_SELF, g0 added with weight 1) must turn s6unit red.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
OUT="${B5S6_OUT:-$WORK/out}"
SEEDS="${B5S6_SEEDS:-123 124 125 126 127 128 129 130}"
JOBS="${B5S6_JOBS:-3}"
mkdir -p "$OUT"
python3 "$HERE/b5_step6_check.py" --self-test > "$OUT/self.log" 2>&1 \
    || { cat "$OUT/self.log"; echo "[result] FAIL  b5-step6 judge self-test"; exit 1; }
echo "[b5-step6] judge self-test ok ($(grep -c ' ok ' "$OUT/self.log") cases)"
L="$ROOT/arch/common/llm"
SRC="$HERE/student_adopt_test.c $L/held_indep.c $L/cradle.c $L/student.c"
build() {  # build <name> <flags...>
    local n="$1"; shift
    # shellcheck disable=SC2086
    $CC $CFLAGS "$@" $SRC -lm -o "$WORK/$n" || { echo "[build] $n FAILED"; exit 1; }
}
echo "[build] b5-step6: plain, flat, nonorm, self ..."
build plain
build flat -DB5S6_TRUST_FLAT
build nonorm -DB5S6_NO_NORM
build self -DB5S6_MUT_SELF
"$WORK/plain" s6unit > "$OUT/unit.log" 2>&1 \
    || { cat "$OUT/unit.log"; echo "[result] FAIL  b5-step6 st_merge_trust self-test"; exit 1; }
echo "[b5-step6] st_merge_trust self-test ok ($(grep -c '  PASS  ' "$OUT/unit.log") checks)"
for m in flat nonorm self; do
    if "$WORK/$m" s6unit > "$OUT/unit-$m.log" 2>&1; then
        echo "[result] FAIL  b5-step6 the $m build passes the self-test"; exit 1
    fi
    echo "[b5-step6] the $m build is red ($(grep -c '  FAIL  ' "$OUT/unit-$m.log") checks)"
done
if [ "${B5S6_MODE:-}" = self ]; then echo "[result] PASS  b5-step6 self-tests"; exit 0; fi
# the long plain parts first, then the NC ones
{ for s in $SEEDS; do echo "plain $s s6a plain-$s-a"; echo "plain $s s6b plain-$s-b"; done
  for s in $SEEDS; do echo "plain $s s6c plain-$s-c"; done
  for s in $SEEDS; do echo "flat $s s6r1 flat-$s"; echo "nonorm $s s6r1 nonorm-$s"; done; } \
    | xargs -P "$JOBS" -L 1 sh -c 'w="$0"; h="$1"; o="$2"; b="$3"; s="$4"; st="$5"; f="$6"; "$w/$b" "$h" "$s" "$st" > "$o/$f.log" 2>&1; echo "[b5-step6] $f rc=$?"' \
        "$WORK" "$HERE/b1_held.txt" "$OUT"
python3 "$HERE/b5_step6_check.py" --seeds "$SEEDS" "$OUT"
