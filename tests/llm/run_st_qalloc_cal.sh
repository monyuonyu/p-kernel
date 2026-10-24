#!/bin/bash
# ---------------------------------------------------------------------------
# run_st_qalloc_cal.sh — [st-qalloc] stage 8 (R-1; docs/architecture/30-module/
#   research/brain-quant-alloc-cal.md §3, gates fixed 2026-10-24 before any
#   run on seeds 25-32). The node builds the three arms and picks the one whose
#   copy loses least bpb on b1_cal.txt (-DST_QALLOC_EL -DST_QALLOC_CAL); held
#   stays values and intervals only. Not in CI.
#
#   plain        -> ALL PASS on every seed (I0 P0 A0 P1 G4 K0 D1; G4 also
#                   b1_cal vs held), "value P2 1" on >= 6/8, and C1: the
#                   clear misses of CAL <= 1 and fewer than omega_el's. A
#                   clear miss = the pick is not the held minimum and the
#                   stage 7 interval excludes 0. No omega_el miss = C1
#                   cannot judge (not green).
#   R            -> seeds 9-16, the stage 6 build: omega_el and damage lines
#                   equal qalloc_el_expect.txt
#   CAL_INVERT   -> P1 FAIL on the first seed (the largest cal damage)
#   CAL_SHORT    -> values only (b1_cal's first 48 windows: the clear misses)
#
#   ./run_st_qalloc_cal.sh      exit 0 = ALL PASS
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla -DST_QALLOC_EL"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SRC="$HERE/student_qalloc_test.c $ROOT/arch/common/llm/held_indep.c $ROOT/arch/common/llm/student.c"
SEEDS="${ST_QALLOC_SEEDS:-25 26 27 28 29 30 31 32}"
R_SEEDS="${ST_QALLOC_R_SEEDS-9 10 11 12 13 14 15 16}"
HELD="$HERE/b1_held.txt"; CAL="$HERE/b1_cal.txt"
FAIL=0

for v in plain CAL_INVERT CAL_SHORT stage6; do
    case "$v" in
        plain)      def="-DST_QALLOC_CAL" ;;
        stage6)     def="" ;;
        *)          def="-DST_QALLOC_CAL -DST_QALLOC_$v" ;;
    esac
    # shellcheck disable=SC2086
    $CC $CFLAGS $def $SRC -lm -o "$WORK/q_$v" || { echo "[build] $v FAILED"; FAIL=1; }
done

first=""; nseed=0; p2=0; mc=0; mo=0
for s in $SEEDS; do
    [ -z "$first" ] && first="$s"
    nseed=$((nseed + 1))
    "$WORK/q_plain" "$HELD" "$s" "$CAL" | tee "$WORK/plain_$s.log"
    grep -q '^\[st-qalloc\] ALL PASS' "$WORK/plain_$s.log" || { echo "[st-qalloc] seed $s RED"; FAIL=1; }
    grep -q '^\[st-qalloc\] value P2 1' "$WORK/plain_$s.log" && p2=$((p2 + 1))
    line=$(grep -a '^\[st-qalloc\] value C1 ' "$WORK/plain_$s.log")
    if [ -z "$line" ]; then echo "[st-qalloc] FAIL C1 seed $s printed no miss line"; FAIL=1; continue; fi
    c=$(echo "$line" | sed -n 's/.*cal miss \([01]\).*/\1/p'); o=$(echo "$line" | sed -n 's/.*omega_el miss \([01]\).*/\1/p')
    mc=$((mc + ${c:-1})); mo=$((mo + ${o:-0}))
done
need=$(( (nseed * 6 + 7) / 8 ))
if [ "$p2" -ge "$need" ]; then
    echo "[st-qalloc] PASS P2 the largest omega is worse than AUTO on $p2/$nseed seeds (need $need)"
else
    echo "[st-qalloc] FAIL P2 the largest omega is worse than AUTO on $p2/$nseed seeds (need $need)"; FAIL=1
fi
if [ "$mo" = 0 ]; then
    echo "[st-qalloc] FAIL C1 cannot judge: omega_el has no clear miss on these seeds (CAL $mc) - take seeds 33-40"; FAIL=1
elif [ "$mc" -le 1 ] && [ "$mc" -lt "$mo" ]; then
    echo "[st-qalloc] PASS C1 clear misses CAL $mc vs omega_el $mo"
else
    echo "[st-qalloc] FAIL C1 clear misses CAL $mc vs omega_el $mo (need CAL <= 1 and fewer)"; FAIL=1
fi

"$WORK/q_CAL_INVERT" "$HELD" "$first" "$CAL" > "$WORK/nc_inv.log" 2>&1
grep -a 'cal damage\|bpb damage' "$WORK/nc_inv.log"
if grep -aq '^\[st-qalloc\] FAIL P1 ' "$WORK/nc_inv.log"; then
    echo "[st-qalloc] PASS negative control CAL_INVERT turns P1 red"
else
    echo "[st-qalloc] FAIL negative control CAL_INVERT did not turn P1 red"; FAIL=1
fi
for s in $SEEDS; do
    "$WORK/q_CAL_SHORT" "$HELD" "$s" "$CAL" 2>&1 | grep -a 'cal damage\|value C1' | sed "s/^\[st-qalloc\] /[st-qalloc] value CAL_SHORT seed $s: /"
done

if [ -n "$R_SEEDS" ]; then
    : > "$WORK/r.out"
    for s in $R_SEEDS; do
        "$WORK/q_stage6" "$HELD" "$s" 2>&1 | grep -aE '^\[st-qalloc\] (omega_el E4|bpb damage seq)' >> "$WORK/r.out"
    done
    if diff "$HERE/qalloc_el_expect.txt" "$WORK/r.out"; then
        echo "[st-qalloc] PASS R seeds $R_SEEDS: $(wc -l < "$WORK/r.out") lines equal the stage 6 record"
    else
        echo "[st-qalloc] FAIL R seeds $R_SEEDS differ from the stage 6 record"; FAIL=1
    fi
fi

if [ "$FAIL" = 0 ]; then echo "[st-qalloc] CERT ALL PASS"; exit 0; fi
echo "[st-qalloc] CERT RED"; exit 1
