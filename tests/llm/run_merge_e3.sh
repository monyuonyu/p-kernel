#!/bin/bash
# ---------------------------------------------------------------------------
# run_merge_e3.sh - the merge-e3 cert (research/brain-merge-e3.md 3): does the
# ss3 W0 pair's plain mean (st_merge_cohort) still lose to the worse parent
# when the parents learn at the product's ST_LR with Adam eps 1e-8?
# student_merge_e3_test.c, judge merge_e3_check.py (E1, NC1, E2). Not in CI.
#
#   ./run_merge_e3.sh               exit 0 = E1 and NC1 green (E2 recorded)
#   ME3_MODE=self                   the judge's self-test only (seconds)
#   ME3_SEEDS="107 ... 114"         the judgement seeds (fixed in the design)
#   ME3_OUT=<dir>  ME3_JOBS=3
# Builds: e8 (student.c as shipped, eps 1e-8) runs arms prod and e2a;
# e64 (-DST_ADAM_EPS=0.015625f, the old root's eps) runs old and e2b.
# Every arm and seed runs twice; the judge wants the two logs byte-identical.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
OUT="${ME3_OUT:-$WORK/out}"
SEEDS="${ME3_SEEDS:-107 108 109 110 111 112 113 114}"
JOBS="${ME3_JOBS:-3}"
mkdir -p "$OUT"
python3 "$HERE/merge_e3_check.py" --self-test > "$OUT/self.log" 2>&1 \
    || { cat "$OUT/self.log"; echo "[result] FAIL  merge-e3 judge self-test"; exit 1; }
echo "[merge-e3] judge self-test ok ($(grep -c '^\[me3-self\] ok ' "$OUT/self.log") cases)"
if [ "${ME3_MODE:-}" = self ]; then echo "[result] PASS  merge-e3 self-test"; exit 0; fi
L="$ROOT/arch/common/llm"
build() {  # build <name> <flags...>
    local n="$1"; shift
    # shellcheck disable=SC2086
    $CC $CFLAGS -I"$L" "$@" "$HERE/student_merge_e3_test.c" "$L/student.c" -lm -o "$WORK/$n" \
        || { echo "[build] $n FAILED"; exit 1; }
}
echo "[build] merge-e3: e8 (as shipped), e64 (-DST_ADAM_EPS=0.015625f) ..."
build e8
build e64 -DST_ADAM_EPS=0.015625f
for s in $SEEDS; do for r in 1 2; do
    echo "e8 prod $s $r"; echo "e8 e2a $s $r"; echo "e64 old $s $r"; echo "e64 e2b $s $r"
done; done | xargs -P "$JOBS" -L 1 sh -c \
    'w="$0"; o="$1"; b="$2"; a="$3"; s="$4"; r="$5"; "$w/$b" "$a" "$s" > "$o/$a-$s-$r.log" 2>&1; echo "[merge-e3] $a seed $s run $r rc=$?"' \
    "$WORK" "$OUT"
if [ "$SEEDS" = "107 108 109 110 111 112 113 114" ]; then
    python3 "$HERE/merge_e3_check.py" "$OUT"
else
    python3 "$HERE/merge_e3_check.py" --seeds "$SEEDS" "$OUT"
fi
