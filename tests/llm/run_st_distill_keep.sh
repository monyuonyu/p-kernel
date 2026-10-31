#!/bin/bash
# ---------------------------------------------------------------------------
# run_st_distill_keep.sh — host cert for the smarter merge, stage 8: parents
#   handed over at their own-data-min round, parents-ok = handed <= 1.0, and
#   the mechanism gate [d5c-swa-bounds] (docs/architecture/30-module/research/
#   brain-merge-keep.md §3, fixed before code).
#
# B_LR 3e-3 for every build. Judgment seeds 22-29 (nobody ran them before the
# design was committed; nothing is chosen on them). Builds (all
# D5C_ONLY_SEL=1, all -DD5C_SWA):
#   plain          -DD5C_STAGE8                               seeds 22-29
#   last           (stage 7's parents: the last weights)      seeds 22-29
#   SEL_INVERT     -DD5C_STAGE8 -DST_DISTILL_SEL_INVERT       seeds 22-29
#   DIV_OFF1       -DD5C_STAGE8 -DST_DISTILL_SWA_DIV_OFF1     seeds 22 23
#   KEEP_FIRST     -DD5C_STAGE8 -DD5C_PARENT_KEEP_FIRST       seeds 22 23
#   PARENT_LR_OLD  -DD5C_STAGE8 -DST_DISTILL_PARENT_LR_OLD    seeds 22 23
# Judges LINES, not exit codes:
#   main (the research verdict)  plain: every start=sel gate line (parents-ok,
#        s1-gain, s2-both, floor, determ, ...), [d5c-sel-val],
#        [d5c-swa-points] and [d5c-swa-bounds] PASS on all 8 seeds
#   secondary (a reading, not judged)  last: seeds with stage 7's parents-ok
#        red; S1 child TEST plain vs last; parents' S1 TEST plain vs last
#   SEL_INVERT  some [d5c-sel-val] FAIL                        (counted)
#   DIV_OFF1    [d5c-swa-bounds] FAIL on S1 and on S2          (counted)
#   KEEP_FIRST  some [d5c-parents-ok] FAIL                     (counted)
#   PARENT_LR_OLD  whether [d5c-parents-ok] bites is printed   (not counted)
# The exit code is 0 iff main is PASS and the three counted NCs bite.
#
#   ./run_st_distill_keep.sh
#   B_JOBS=4 D5C_JUDGE_SEEDS="22" D5C_NC_SEEDS="22" ./run_st_distill_keep.sh
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
SEEDS="${D5C_JUDGE_SEEDS:-22 23 24 25 26 27 28 29}"
NC_SEEDS="${D5C_NC_SEEDS:-22 23}"
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
gates() {   # $1 = log: start=sel gate lines, sel-val, swa-points, swa-bounds
    grep -E '^  (PASS|FAIL)  ' "$1" 2>/dev/null |
        grep -E 'start=sel|\[d5c-sel-val\]|\[d5c-swa-points\]|\[d5c-swa-bounds\]'
}
s1test() {  # $1 = log -> "A B child" of the S1 sel TEST line
    grep -E '^  \[T-real S1 sel\] TEST ' "$1" 2>/dev/null |
        sed -E 's/.* A=([0-9.]+) B=([0-9.]+) child=([0-9.]+) .*/\1 \2 \3/'
}

build plain -DD5C_STAGE8
build last
build SEL_INVERT -DD5C_STAGE8 -DST_DISTILL_SEL_INVERT
build DIV_OFF1 -DD5C_STAGE8 -DST_DISTILL_SWA_DIV_OFF1
build KEEP_FIRST -DD5C_STAGE8 -DD5C_PARENT_KEEP_FIRST
build PARENT_LR_OLD -DD5C_STAGE8 -DST_DISTILL_PARENT_LR_OLD

# plain first so the verdict's logs are done earliest
{
    for v in plain last SEL_INVERT; do
        for s in $SEEDS; do echo "$v $s"; done
    done
    for v in DIV_OFF1 KEEP_FIRST PARENT_LR_OLD; do
        for s in $NC_SEEDS; do echo "$v $s"; done
    done
} > "$WORK/jobs.txt"
echo "[run] $(grep -c . "$WORK/jobs.txt") runs, $JOBS at a time"
# shellcheck disable=SC2016
xargs -P "$JOBS" -L 1 sh -c '[ -x "$0/$1" ] && D5C_SEED="$2" D5C_ONLY_SEL=1 "$0/$1" > "$0/$1-$2.log" 2>&1; echo "[done] $1 $2 $(date -Is)"' "$WORK" < "$WORK/jobs.txt"

# ---- main ----
main=0
for s in $SEEDS; do
    echo "=== judge plain seed $s ==="
    grep -E '^  \[(T-real|sel|walk|parents|swa-bounds|d5c-teacher-reach)' "$WORK/plain-$s.log"
    gates "$WORK/plain-$s.log"
    n=$(gates "$WORK/plain-$s.log" | grep -c .)
    nf=$(gates "$WORK/plain-$s.log" | grep -c '^  FAIL')
    nb=$(gates "$WORK/plain-$s.log" | grep -c '\[d5c-swa-bounds\]')
    if [ "$n" -gt 0 ] && [ "$nf" -eq 0 ] && [ "$nb" -eq 2 ]; then
        echo "[result] plain seed $s GREEN ($n lines)"
    else
        echo "[result] plain seed $s RED ($nf/$n FAIL, swa-bounds lines $nb)"; main=1
    fi
done

# ---- secondary (a reading) ----
echo ""
red7=""; better=0; worse=0; moved=0
for s in $SEEDS; do
    grep -E '^  FAIL  \[d5c-parents-ok\].*start=sel' "$WORK/last-$s.log" >/dev/null 2>&1 && red7="$red7 $s"
    p=$(s1test "$WORK/plain-$s.log"); l=$(s1test "$WORK/last-$s.log")
    echo "  [secondary seed $s] S1 TEST (A B child) plain: ${p:-na} | last: ${l:-na}"
    w=$(python3 -c 'import sys
p=sys.argv[1].split(); l=sys.argv[2].split()
if len(p)<3 or len(l)<3: print("na"); sys.exit()
mv = "moved" if (p[0],p[1]) != (l[0],l[1]) else "same"
d = float(p[2]) - float(l[2])
print(mv, "better" if d < 0 else ("worse" if d > 0 else "equal"), "%+.4f" % d)' "$p" "$l")
    echo "    -> parents $w (child plain - last)"
    case "$w" in moved*) moved=$((moved + 1));; esac
    case "$w" in *better*) better=$((better + 1));; *worse*) worse=$((worse + 1));; esac
done
echo "[result] secondary: stage 7's parents-ok red on seeds:${red7:- none}; parents moved on $moved seeds; S1 child better on $better, worse on $worse (a reading, not judged)"

# ---- NCs ----
nc() {   # $1 = variant, $2 = grep -E pattern, $3 = seeds -> prints hits, sets NCL
    NCL=""
    for s in $3; do
        local l
        l=$(gates "$WORK/$1-$s.log" | grep -E "$2")
        printf '%s\n' "$l" | sed "s/^/  [$1 seed $s]/"
        NCL="$NCL
$l"
    done
}
echo ""
nc SEL_INVERT '\[d5c-sel-val\]' "$SEEDS"
if printf '%s\n' "$NCL" | grep -q '^  FAIL'; then
    echo "[result] SEL_INVERT RED on target - negative control bites"
else echo "[result] SEL_INVERT did NOT go red on target"; rc_all=1; fi
nc DIV_OFF1 '\[d5c-swa-bounds\]' "$NC_SEEDS"
if printf '%s\n' "$NCL" | grep -q '^  FAIL  \[d5c-swa-bounds\] S1' &&
   printf '%s\n' "$NCL" | grep -q '^  FAIL  \[d5c-swa-bounds\] S2'; then
    echo "[result] DIV_OFF1 RED on target (S1 and S2) - negative control bites"
else echo "[result] DIV_OFF1 did NOT go red on S1 and S2"; rc_all=1; fi
nc KEEP_FIRST '\[d5c-parents-ok\]' "$NC_SEEDS"
if printf '%s\n' "$NCL" | grep -q '^  FAIL'; then
    echo "[result] KEEP_FIRST RED on target - negative control bites"
else echo "[result] KEEP_FIRST did NOT go red on target"; rc_all=1; fi
nc PARENT_LR_OLD '\[d5c-parents-ok\]' "$NC_SEEDS"
if printf '%s\n' "$NCL" | grep -q '^  FAIL'; then
    echo "[result] PARENT_LR_OLD: parents-ok bites (recorded, not counted)"
else echo "[result] PARENT_LR_OLD: parents-ok does not bite (recorded, not counted)"; fi

miss=""
for s in $SEEDS; do
    grep -q 'd5c-teacher-reach.*-> MISS' "$WORK/plain-$s.log" 2>/dev/null && miss="$miss $s"
done
echo "[result] teacher-reach MISS on seeds:${miss:- none} (recorded, not counted)"

if [ "$main" -eq 0 ]; then echo "[result] main PASS"; else echo "[result] main RED"; fi
if [ "$main" -eq 0 ] && [ "$rc_all" -eq 0 ]; then echo "[st-distill-keep] PASS"; else echo "[st-distill-keep] FAIL"; rc_all=1; fi
exit "$rc_all"
