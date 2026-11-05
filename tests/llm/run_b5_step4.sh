#!/bin/bash
# ---------------------------------------------------------------------------
# run_b5_step4.sh - B5 step 4 (research/b5-step4-design.md): every arm stops at
# node A's best pass on the gate window; the honest peer + apprentice against
# one node shrunk x0.5 through the same gate. student_adopt_test.c stage s4,
# judge b5_step4_check.py (K1-K4, NC). Not in CI (about 10 min a seed a build).
#
#   ./run_b5_step4.sh               exit 0 = K1-K4 green and NC red
#   B5S4_MODE=self                  the judge's self-test only (seconds)
#   B5S4_SEEDS="99 ... 106"         the judgement seeds (fixed in the design)
#   B5S4_OUT=<dir>  B5S4_JOBS=3
# Builds: plain, and same (-DB5S4_PEER_SAME: the peer learns A's own half
# shifted by SEQ/2, no new bytes) which must turn K1 red.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
OUT="${B5S4_OUT:-$WORK/out}"
SEEDS="${B5S4_SEEDS:-99 100 101 102 103 104 105 106}"
JOBS="${B5S4_JOBS:-3}"
mkdir -p "$OUT"
python3 "$HERE/b5_step4_check.py" --self-test > "$OUT/self.log" 2>&1 \
    || { cat "$OUT/self.log"; echo "[result] FAIL  b5-step4 judge self-test"; exit 1; }
echo "[b5-step4] judge self-test ok ($(grep -c ' ok ' "$OUT/self.log") cases)"
if [ "${B5S4_MODE:-}" = self ]; then echo "[result] PASS  b5-step4 self-test"; exit 0; fi
L="$ROOT/arch/common/llm"
SRC="$HERE/student_adopt_test.c $L/held_indep.c $L/cradle.c $L/student.c"
build() {  # build <name> <flags...>
    local n="$1"; shift
    # shellcheck disable=SC2086
    $CC $CFLAGS "$@" $SRC -lm -o "$WORK/$n" || { echo "[build] $n FAILED"; exit 1; }
}
echo "[build] b5-step4: plain, same (-DB5S4_PEER_SAME) ..."
build plain
build same -DB5S4_PEER_SAME
for s in $SEEDS; do for b in plain same; do echo "$b $s"; done; done \
    | xargs -P "$JOBS" -L 1 sh -c 'w="$0"; h="$1"; o="$2"; b="$3"; s="$4"; "$w/$b" "$h" "$s" s4 > "$o/$b-$s.log" 2>&1; echo "[b5-step4] $b seed $s rc=$?"' \
        "$WORK" "$HERE/b1_held.txt" "$OUT"
python3 "$HERE/b5_step4_check.py" --seeds "$SEEDS" "$OUT"
