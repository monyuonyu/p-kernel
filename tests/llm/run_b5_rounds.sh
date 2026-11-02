#!/bin/bash
# ---------------------------------------------------------------------------
# run_b5_rounds.sh - B5 stage 1: averaging every 5 passes, one process, no net
#   (docs/architecture/30-module/research/b5-region-rounds.md, section 4).
#
#   B5_MODE=gates ./run_b5_rounds.sh   seconds: G4, G5, the judge's self-test
#                                      and -DST_ROUND_NO_GEN, -DST_MERGE_NO_FINITE
#   ./run_b5_rounds.sh                 the cert on the judgment seeds 43..50:
#                                      plain, B5_PEER=reset, B5_PEER=random and
#                                      stage 9's keep child on the same seeds
#   B5_WORK=/build/x B_JOBS=3          keep the logs there, arms side by side
#   B5_SEEDS="4 5"                     other seeds, for a look (not a verdict)
#
# Exit 0 = everything green. A red G1, G2 or G3 is a research red: no retake.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
WORK="${B5_WORK:-}"
if [ -z "$WORK" ]; then WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT; fi
mkdir -p "$WORK"
SRC="$HERE/student_round_test.c $ROOT/arch/common/llm/student.c"
SEEDS="${B5_SEEDS:-43 44 45 46 47 48 49 50}"
JOBS="${B_JOBS:-3}"
RC=0

echo "[build] plain and two falsifiers ..."
# shellcheck disable=SC2086
$CC $CFLAGS $SRC -lm -o "$WORK/plain" || { echo "[build] FAILED"; exit 1; }
# shellcheck disable=SC2086
$CC $CFLAGS -DST_ROUND_NO_GEN $SRC -lm -o "$WORK/nogen" || { echo "[build] FAILED"; exit 1; }
# shellcheck disable=SC2086
$CC $CFLAGS -DST_MERGE_NO_FINITE $SRC -lm -o "$WORK/nofin" || { echo "[build] FAILED"; exit 1; }

echo "=== gates (must PASS) ==="
"$WORK/plain" gates; [ "$?" = 0 ] || RC=1

nc() { # <binary> <the line that must be red>
    "$WORK/$1" gates > "$WORK/$1.out" 2>&1
    if grep -qF "  FAIL  [$2]" "$WORK/$1.out" && grep -q '^  PASS  ' "$WORK/$1.out"; then
        echo "  ok    $1: [$2] is red"
    else
        echo "  WRONG $1: [$2] should be red"; RC=1
    fi
}
nc nogen b5-gen
nc nofin b5-nonfinite

python3 "$HERE/b5_judge.py" --self | tail -1
[ "${PIPESTATUS[0]}" = 0 ] || RC=1

if [ "${B5_MODE:-cert}" = gates ]; then
    [ "$RC" = 0 ] && echo "[result] PASS  b5 gates" || echo "[result] FAIL  b5 gates"
    exit "$RC"
fi

echo "[build] stage 9's keep child ..."
# shellcheck disable=SC2086
$CC $CFLAGS -DD5C_B_LR=3e-3f -DD5C_SWA -DD5C_STAGE8 -DD5C_STAGE9 \
    "$HERE/student_distill_merge_test.c" "$ROOT/arch/common/llm/student.c" -lm -o "$WORK/keep" \
    || { echo "[build] FAILED"; exit 1; }

: > "$WORK/jobs.txt"
for s in $SEEDS; do
    { echo "plain $s"; echo "keep $s"; echo "reset $s"; echo "random $s"; } >> "$WORK/jobs.txt"
done
echo "[run] $(grep -c . "$WORK/jobs.txt") runs, $JOBS at a time"
# shellcheck disable=SC2016
xargs -P "$JOBS" -L 1 sh -c '
    if [ "$1" = keep ]; then
        D5C_SEED="$2" D5C_ONLY_SEL=1 "$0/keep" > "$0/keep-$2.log" 2>&1
    elif [ "$1" = plain ]; then
        "$0/plain" run "$2" > "$0/plain-$2.log" 2>&1
    else
        B5_PEER="$1" "$0/plain" run "$2" > "$0/$1-$2.log" 2>&1
    fi
    echo "[done] $1 $2 $(date -Is)"' "$WORK" < "$WORK/jobs.txt"

python3 "$HERE/b5_judge.py" "$WORK" "$SEEDS" || RC=1
[ "$RC" = 0 ] && echo "[result] PASS  b5 rounds" || echo "[result] FAIL  b5 rounds"
exit "$RC"
