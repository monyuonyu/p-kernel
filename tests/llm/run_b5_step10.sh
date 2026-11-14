#!/bin/bash
# ---------------------------------------------------------------------------
# run_b5_step10.sh - B5 step 10 (research/b5-step10-design.md): step 9's arms
# with the trust arms on st_merge_trust_group (one voice per group of
# same-direction peers). student_adopt_test.c stages s10a s10b s10y s10c s10d
# s10p s10r1 s10r2, step 9 (s9a s9c, st_merge_trust_echo) of the same build
# for R10, the NC1 build (-DB5S10_GROUP_FAR) stage s10n, the NC2 build
# (-DB5S10_TG_LOW) stage s10a; judge b5_step10_check.py. Not in CI (about two
# and a half hours on 3 cores).
#
#   ./run_b5_step10.sh              exit 0 = self-tests green, R1-R10 green, NC1 and NC2 red
#   B5S10_MODE=self                 the judge's self-test and the units (seconds)
#   B5S10_SEEDS="157 ... 164"       the judgement seeds (fixed in the design)
#   B5S10_OUT=<dir>  B5S10_JOBS=3
# The jobs R7' and the NCs need are started first, seed by seed.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
OUT="${B5S10_OUT:-$WORK/out}"
SEEDS="${B5S10_SEEDS:-157 158 159 160 161 162 163 164}"
JOBS="${B5S10_JOBS:-3}"
mkdir -p "$OUT"
python3 "$HERE/b5_step10_check.py" --self-test > "$OUT/self.log" 2>&1 \
    || { cat "$OUT/self.log"; echo "[result] FAIL  b5-step10 judge self-test"; exit 1; }
echo "[b5-step10] judge self-test ok ($(grep -c ' ok ' "$OUT/self.log") cases)"
L="$ROOT/arch/common/llm"
SRC="$HERE/student_adopt_test.c $L/held_indep.c $L/cradle.c $L/student.c"
build() {  # build <name> <flags...>
    local n="$1"; shift
    # shellcheck disable=SC2086
    $CC $CFLAGS "$@" $SRC -lm -o "$WORK/$n" || { echo "[build] $n FAILED"; exit 1; }
}
echo "[build] b5-step10: plain, far, tglow ..."
build plain
build far -DB5S10_GROUP_FAR
build tglow -DB5S10_TG_LOW
for u in s6unit s7unit s8unit s10unit; do
    "$WORK/plain" $u > "$OUT/unit-$u.log" 2>&1 \
        || { cat "$OUT/unit-$u.log"; echo "[result] FAIL  b5-step10 $u"; exit 1; }
    echo "[b5-step10] $u ok ($(grep -c '  PASS  ' "$OUT/unit-$u.log") checks)"
done
for b in far tglow; do
    if "$WORK/$b" s10unit > "$OUT/unit-$b.log" 2>&1; then
        echo "[result] FAIL  b5-step10 the $b build passes s10unit"; exit 1
    fi
    echo "[b5-step10] the $b build is red on s10unit ($(grep -c '  FAIL  ' "$OUT/unit-$b.log") checks)"
done
if [ "${B5S10_MODE:-}" = self ]; then echo "[result] PASS  b5-step10 self-tests"; exit 0; fi
{ for s in $SEEDS; do for p in p b r1 r2; do echo "plain $s s10$p plain-$s-$p"; done
      echo "far $s s10n far-$s"; echo "tglow $s s10a tglow-$s"; done
  for s in $SEEDS; do for p in a y c d; do echo "plain $s s10$p plain-$s-$p"; done
      echo "plain $s s9a echo-$s-a"; echo "plain $s s9c echo-$s-c"; done; } \
    | xargs -P "$JOBS" -L 1 sh -c 'w="$0"; h="$1"; o="$2"; b="$3"; s="$4"; st="$5"; f="$6"; "$w/$b" "$h" "$s" "$st" > "$o/$f.log" 2>&1; echo "[b5-step10] $f rc=$?"' \
        "$WORK" "$HERE/b1_held.txt" "$OUT"
python3 "$HERE/b5_step10_check.py" --seeds "$SEEDS" "$OUT"
