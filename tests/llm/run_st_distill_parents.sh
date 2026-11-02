#!/bin/bash
# ---------------------------------------------------------------------------
# run_st_distill_parents.sh — host cert for the smarter merge, stage 9: can a
#   child tell stage 8's parents from stage 7's, and is the min round the
#   reason (docs/architecture/30-module/research/brain-merge-parents.md §3,
#   fixed before code and before any child ran on the judgment seeds).
#
# B_LR 3e-3 for every build. Judgment seeds 30-37 and 39-42, picked from the
# parents alone. Builds (all D5C_ONLY_SEL=1, all -DD5C_SWA):
#   keep    -DD5C_STAGE8 -DD5C_STAGE9                      all seeds
#   last    (stage 7's parents: the last weights)          all seeds
#   near    -DD5C_STAGE8 -DD5C_STAGE9 -DD5C_PARENT_NEAR    all seeds, S1 only
#   NOKEEP  -DD5C_STAGE8 -DD5C_STAGE9 -DD5C_PARENT_NOKEEP  seeds 30 31
# d5c9_judge.py reads the logs (lines, not exit codes) and prints the verdict;
# the exit code is its exit code. Self-test of the judge: d5c9_selftest.py.
#
#   D5C_WORK=/build/x B_JOBS=4 ./run_st_distill_parents.sh
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
WORK="${D5C_WORK:-}"
if [ -z "$WORK" ]; then WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT; fi
mkdir -p "$WORK"

SRC_TEST="$HERE/student_distill_merge_test.c"
SRC_STU="$ROOT/arch/common/llm/student.c"
SEEDS="${D5C_JUDGE_SEEDS:-30 31 32 33 34 35 36 37 39 40 41 42}"
NC_SEEDS="${D5C_NC_SEEDS:-30 31}"
JOBS="${B_JOBS:-4}"
BASE="-DD5C_B_LR=3e-3f -DD5C_SWA"

rc_all=0
build() {   # $1 = out name, $2... = extra flags
    local o="$1"; shift
    echo "[build] $o $*"
    if ! $CC $CFLAGS $BASE "$@" "$SRC_TEST" "$SRC_STU" -lm -o "$WORK/$o"; then
        echo "[build] $o FAILED"; rc_all=1
    fi
}

build keep -DD5C_STAGE8 -DD5C_STAGE9
build last
build near -DD5C_STAGE8 -DD5C_STAGE9 -DD5C_PARENT_NEAR
build NOKEEP -DD5C_STAGE8 -DD5C_STAGE9 -DD5C_PARENT_NOKEEP

# keep and last first: the verdict's parents are done earliest
{
    for v in keep last near; do
        for s in $SEEDS; do echo "$v $s"; done
    done
    for s in $NC_SEEDS; do echo "NOKEEP $s"; done
} > "$WORK/jobs.txt"
echo "[run] $(grep -c . "$WORK/jobs.txt") runs, $JOBS at a time"
# shellcheck disable=SC2016
xargs -P "$JOBS" -L 1 sh -c 'o=0; [ "$1" = near ] && o=1; [ -x "$0/$1" ] && D5C_SEED="$2" D5C_ONLY_SEL=1 D5C_ONLY_S1="$o" "$0/$1" > "$0/$1-$2.log" 2>&1; echo "[done] $1 $2 $(date -Is)"' "$WORK" < "$WORK/jobs.txt"

for s in $SEEDS; do
    echo "=== keep seed $s ==="
    grep -E '^  \[(parents|keep-min)' "$WORK/keep-$s.log"
done
python3 "$HERE/d5c9_judge.py" "$WORK" "$SEEDS" "$NC_SEEDS"
rc=$?
[ "$rc_all" -eq 0 ] || rc=1
exit "$rc"
