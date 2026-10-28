#!/bin/bash
# ---------------------------------------------------------------------------
# run_st_distill_swa.sh — host cert for the smarter merge, stage 7: the init
#   arm returns the mean of its second-half weights (st_merge_distill_sel_swa
#   in arch/common/llm/student.c; docs/architecture/30-module/research/
#   brain-merge-swa.md §3, gates fixed before code).
#
# B_LR 3e-3 for every build (fixed on the seen seeds; nothing is chosen on
# the judgment seeds 14-21). Builds (all D5C_ONLY_SEL=1):
#   plain          -DD5C_SWA                         seeds 14-21
#   cmp            stage-6 form (VAL-best init arm)  seeds 14-21
#   SWA_FIRST      -DD5C_SWA -DST_DISTILL_SWA_FIRST  seeds 14-21
#   SEL_INVERT     -DD5C_SWA -DST_DISTILL_SEL_INVERT seeds 14-21
#   PARENT_LR_OLD  -DD5C_SWA -DST_DISTILL_PARENT_LR_OLD  seeds 14 15
# Judges LINES, not exit codes:
#   main (the research verdict)  plain: every start=sel gate line,
#        [d5c-sel-val] and [d5c-swa-points] PASS on all 8 seeds
#   secondary (improvement?)     S1 child TEST lower in plain than cmp on
#        >= 6 seeds AND plain's s1-gain PASS count >= cmp's
#   SEL_INVERT     some [d5c-sel-val] FAIL                  (counted)
#   PARENT_LR_OLD  some [d5c-parents-ok] start=sel FAIL     (counted)
#   SWA_FIRST      S1 child TEST worse than plain on >= 6 seeds -> "the
#        second half matters"; else that is written (recorded, not counted)
#   [d5c-teacher-reach] MISS seeds are named (recorded, not counted)
#   the init arm must differ between plain and cmp ([walk S init] lines),
#   or -DD5C_SWA did nothing and the cert is RED.
# The exit code is 0 iff main is PASS and both counted NCs bite.
#
#   ./run_st_distill_swa.sh
#   B_JOBS=4 D5C_JUDGE_SEEDS="14" D5C_PLR_SEEDS="14" ./run_st_distill_swa.sh
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
SEEDS="${D5C_JUDGE_SEEDS:-14 15 16 17 18 19 20 21}"
PLR_SEEDS="${D5C_PLR_SEEDS:-14 15}"
JOBS="${B_JOBS:-4}"
BLR="-DD5C_B_LR=3e-3f"

rc_all=0
build() {   # $1 = out name, $2... = extra flags
    local o="$1"; shift
    echo "[build] $o $*"
    if ! $CC $CFLAGS $BLR "$@" "$SRC_TEST" "$SRC_STU" -lm -o "$WORK/$o"; then
        echo "[build] $o FAILED"; rc_all=1
    fi
}
gates() {   # $1 = log: start=sel gate lines, sel-val and swa-points lines
    grep -E '^  (PASS|FAIL)  ' "$1" 2>/dev/null | grep -E 'start=sel|\[d5c-sel-val\]|\[d5c-swa-points\]'
}
s1child() { # $1 = log -> the S1 sel child's TEST
    grep -E '^  \[T-real S1 sel\] TEST ' "$1" 2>/dev/null | sed -E 's/.* child=([0-9.]+) .*/\1/'
}

build plain -DD5C_SWA
build cmp
build SWA_FIRST -DD5C_SWA -DST_DISTILL_SWA_FIRST
build SEL_INVERT -DD5C_SWA -DST_DISTILL_SEL_INVERT
build PARENT_LR_OLD -DD5C_SWA -DST_DISTILL_PARENT_LR_OLD

# the job list, longest-lived first is not known; plain first so the
# verdict's logs are done earliest
{
    for v in plain cmp SWA_FIRST SEL_INVERT; do
        for s in $SEEDS; do echo "$v $s"; done
    done
    for s in $PLR_SEEDS; do echo "PARENT_LR_OLD $s"; done
} > "$WORK/jobs.txt"
echo "[run] $(grep -c . "$WORK/jobs.txt") runs, $JOBS at a time"
# shellcheck disable=SC2016
xargs -P "$JOBS" -L 1 sh -c '[ -x "$0/$1" ] && D5C_SEED="$2" D5C_ONLY_SEL=1 "$0/$1" > "$0/$1-$2.log" 2>&1; echo "[done] $1 $2 $(date -Is)"' "$WORK" < "$WORK/jobs.txt"

# ---- main ----
main=0
for s in $SEEDS; do
    echo "=== judge plain seed $s ==="
    grep -E '^  \[(T-real|sel|walk|parents|d5c-teacher-reach)' "$WORK/plain-$s.log"
    gates "$WORK/plain-$s.log"
    n=$(gates "$WORK/plain-$s.log" | grep -c .)
    nf=$(gates "$WORK/plain-$s.log" | grep -c '^  FAIL')
    if [ "$n" -gt 0 ] && [ "$nf" -eq 0 ]; then
        echo "[result] plain seed $s GREEN ($n lines)"
    else
        echo "[result] plain seed $s RED ($nf/$n FAIL)"; main=1
    fi
done

# ---- the flag reached the init arm ----
eff=0
for s in $SEEDS; do
    a=$(grep -E '^  \[walk [^ ]+ init\] ' "$WORK/plain-$s.log" | sed -E 's/ swa_points=[0-9]+$//')
    b=$(grep -E '^  \[walk [^ ]+ init\] ' "$WORK/cmp-$s.log")
    [ -n "$a" ] && [ -n "$b" ] && [ "$a" != "$b" ] && eff=1
done
if [ "$eff" -ne 1 ]; then
    echo "[result] RED: the init arm's [walk] lines are the same in plain and cmp - -DD5C_SWA did nothing"
    main=1
fi

# ---- secondary ----
better=0; pg=0; cg=0
for s in $SEEDS; do
    p=$(s1child "$WORK/plain-$s.log"); c=$(s1child "$WORK/cmp-$s.log")
    w=$(python3 -c 'import sys; a,b=sys.argv[1:3]; print("plain" if a and b and float(a) < float(b) else "no")' "$p" "$c")
    echo "  [secondary seed $s] S1 child TEST plain=${p:-na} cmp=${c:-na} -> lower: $w"
    [ "$w" = plain ] && better=$((better + 1))
    pg=$((pg + $(gates "$WORK/plain-$s.log" | grep -c '^  PASS  \[d5c-s1-gain\]')))
    cg=$((cg + $(grep -E '^  PASS  \[d5c-s1-gain\].*start=sel' "$WORK/cmp-$s.log" | grep -c .)))
done
echo "  [secondary] plain lower on $better seeds; s1-gain PASS plain $pg vs cmp $cg"
if [ "$better" -ge 6 ] && [ "$pg" -ge "$cg" ]; then
    echo "[result] secondary: improvement"
else
    echo "[result] secondary: not shown to be an improvement"
fi

# ---- NCs ----
nc() {   # $1 = variant, $2 = grep -E pattern, $3 = seeds
    local hit=0
    for s in $3; do
        local l
        l=$(gates "$WORK/$1-$s.log" | grep -E "$2")
        printf '%s\n' "$l" | sed "s/^/  [$1 seed $s]/"
        if printf '%s\n' "$l" | grep -q '^  FAIL'; then hit=1; fi
    done
    if [ "$hit" -eq 1 ]; then
        echo "[result] $1 RED on target - negative control bites"
    else
        echo "[result] $1 did NOT go red on target"; rc_all=1
    fi
}
echo ""
nc SEL_INVERT '\[d5c-sel-val\]' "$SEEDS"
nc PARENT_LR_OLD '\[d5c-parents-ok\]' "$PLR_SEEDS"

worse=0
for s in $SEEDS; do
    p=$(s1child "$WORK/plain-$s.log"); f=$(s1child "$WORK/SWA_FIRST-$s.log")
    w=$(python3 -c 'import sys; a,b=sys.argv[1:3]; print("yes" if a and b and float(b) > float(a) else "no")' "$p" "$f")
    echo "  [SWA_FIRST seed $s] S1 child TEST plain=${p:-na} first-half=${f:-na} -> first-half worse: $w"
    [ "$w" = yes ] && worse=$((worse + 1))
done
if [ "$worse" -ge 6 ]; then
    echo "[result] SWA_FIRST worse on $worse seeds - the second half matters (recorded, not counted)"
else
    echo "[result] SWA_FIRST worse on only $worse seeds - not shown that the second half matters (recorded, not counted)"
fi

miss=""
for s in $SEEDS; do
    grep -q 'd5c-teacher-reach.*-> MISS' "$WORK/plain-$s.log" 2>/dev/null && miss="$miss $s"
done
echo "[result] teacher-reach MISS on seeds:${miss:- none} (recorded, not counted)"

if [ "$main" -eq 0 ]; then echo "[result] main PASS"; else echo "[result] main RED"; fi
if [ "$main" -eq 0 ] && [ "$rc_all" -eq 0 ]; then echo "[st-distill-swa] PASS"; else echo "[st-distill-swa] FAIL"; rc_all=1; fi
exit "$rc_all"
