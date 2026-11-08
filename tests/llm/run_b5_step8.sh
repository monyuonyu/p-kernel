#!/bin/bash
# ---------------------------------------------------------------------------
# run_b5_step8.sh - B5 step 8 (research/b5-step8-design.md): step 7's four
# nodes; a peer whose destination is an echo of the receiver's own update
# (cos(gj, g0) >= 0.9) gets TS 0 (st_merge_trust_echo). student_adopt_test.c
# stages s8unit and s8a/s8b/s8y/s8c/s8d, judge b5_step8_check.py (R1-R9,
# NC1, NC2). Not in CI (tens of minutes a seed).
#
#   ./run_b5_step8.sh               exit 0 = self-tests green, R1-R9 green, NC1 NC2 red
#   B5S8_MODE=self                  the two self-tests and the two NC builds' units (seconds)
#   B5S8_SEEDS="141 ... 148"        the judgement seeds (fixed in the design)
#   B5S8_OUT=<dir>  B5S8_JOBS=3
# Builds: plain; noecho (-DB5S8_NO_ECHO, step 7, NC1) runs s8b; taulow
# (-DB5S8_TAU_LOW, tau 0.1, NC2) runs s8a. Both must turn s8unit red.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
OUT="${B5S8_OUT:-$WORK/out}"
SEEDS="${B5S8_SEEDS:-141 142 143 144 145 146 147 148}"
JOBS="${B5S8_JOBS:-3}"
mkdir -p "$OUT"
python3 "$HERE/b5_step8_check.py" --self-test > "$OUT/self.log" 2>&1 \
    || { cat "$OUT/self.log"; echo "[result] FAIL  b5-step8 judge self-test"; exit 1; }
echo "[b5-step8] judge self-test ok ($(grep -c ' ok ' "$OUT/self.log") cases)"
L="$ROOT/arch/common/llm"
SRC="$HERE/student_adopt_test.c $L/held_indep.c $L/cradle.c $L/student.c"
build() {  # build <name> <flags...>
    local n="$1"; shift
    # shellcheck disable=SC2086
    $CC $CFLAGS "$@" $SRC -lm -o "$WORK/$n" || { echo "[build] $n FAILED"; exit 1; }
}
echo "[build] b5-step8: plain, noecho, taulow ..."
build plain
build noecho -DB5S8_NO_ECHO
build taulow -DB5S8_TAU_LOW
for u in s6unit s7unit s8unit; do
    "$WORK/plain" $u > "$OUT/unit-$u.log" 2>&1 \
        || { cat "$OUT/unit-$u.log"; echo "[result] FAIL  b5-step8 $u"; exit 1; }
    echo "[b5-step8] $u ok ($(grep -c '  PASS  ' "$OUT/unit-$u.log") checks)"
done
for b in noecho taulow; do
    if "$WORK/$b" s8unit > "$OUT/unit-$b.log" 2>&1; then
        echo "[result] FAIL  b5-step8 the $b build passes s8unit"; exit 1
    fi
    echo "[b5-step8] the $b build is red ($(grep -c '  FAIL  ' "$OUT/unit-$b.log") checks)"
done
if [ "${B5S8_MODE:-}" = self ]; then echo "[result] PASS  b5-step8 self-tests"; exit 0; fi
{ for s in $SEEDS; do echo "plain $s s8a plain-$s-a"; echo "plain $s s8b plain-$s-b"; done
  for s in $SEEDS; do echo "plain $s s8y plain-$s-y"; echo "plain $s s8c plain-$s-c"; echo "plain $s s8d plain-$s-d"; done
  for s in $SEEDS; do echo "noecho $s s8b noecho-$s"; echo "taulow $s s8a taulow-$s"; done; } \
    | xargs -P "$JOBS" -L 1 sh -c 'w="$0"; h="$1"; o="$2"; b="$3"; s="$4"; st="$5"; f="$6"; "$w/$b" "$h" "$s" "$st" > "$o/$f.log" 2>&1; echo "[b5-step8] $f rc=$?"' \
        "$WORK" "$HERE/b1_held.txt" "$OUT"
python3 "$HERE/b5_step8_check.py" --seeds "$SEEDS" "$OUT"
