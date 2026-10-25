#!/bin/bash
# ---------------------------------------------------------------------------
# run_st_qalloc_src.sh — [st-qalloc] stage 9 (R-1; docs/architecture/30-module/
#   research/brain-quant-alloc-src.md §3, gates fixed 2026-10-25 before any
#   run on seeds 33-48). The pick stays stage 8's CAL; on the same three arms
#   the build also prints the damage on three other texts and the arm each
#   would pick (-DST_QALLOC_SRC): S33 (b1_cal's first 33 windows), TAIL (the
#   fixture's 33 windows after train_end, never trained), GATE (the first 33
#   gate windows, trained). Held stays values and intervals only. Not in CI.
#
#   plain      -> ALL PASS on every seed (I0 P0 A0 P1 G4 K0 D1; G4 also TAIL
#                 vs held and b1_cal vs the fixture), "value P2 1" on >= 12/16,
#                 three "value SRC" lines per seed, and C1: the clear misses of
#                 CAL <= 2 and fewer than omega_el's (omega_el 0 = cannot
#                 judge, not green). H is a reading, printed, never a gate.
#   CAL_INVERT -> P1 FAIL on the first seed
#   SRC_SWAP   -> the TAIL line reads S33's windows: its numbers equal the
#                 S33 line's on the first seed (and the plain build's differ)
#   R          -> seeds 25-32: the cal damage and C1 lines equal stage 8's
#                 record (qalloc_cal_expect.txt, audit-81's run)
#
#   ST_QALLOC_JOBS=n runs n seeds at once (the output is the same).
#   ./run_st_qalloc_src.sh      exit 0 = ALL PASS
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla -DST_QALLOC_EL -DST_QALLOC_CAL -DST_QALLOC_SRC"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SRC="$HERE/student_qalloc_test.c $ROOT/arch/common/llm/held_indep.c $ROOT/arch/common/llm/student.c"
SEEDS="${ST_QALLOC_SEEDS:-33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48}"
R_SEEDS="${ST_QALLOC_R_SEEDS-25 26 27 28 29 30 31 32}"
JOBS="${ST_QALLOC_JOBS:-1}"
HELD="$HERE/b1_held.txt"; CAL="$HERE/b1_cal.txt"
FAIL=0

for v in plain CAL_INVERT SRC_SWAP; do
    case "$v" in
        plain) def="" ;;
        *)     def="-DST_QALLOC_$v" ;;
    esac
    # shellcheck disable=SC2086
    $CC $CFLAGS $def $SRC -lm -o "$WORK/q_$v" || { echo "[build] $v FAILED"; FAIL=1; }
done

# run "<bin> <seed> <log>" lines, JOBS at a time
runq() {
    local n=0
    while read -r bin s log; do
        "$WORK/$bin" "$HELD" "$s" "$CAL" > "$WORK/$log" 2>&1 &
        n=$((n + 1))
        if [ "$n" -ge "$JOBS" ]; then wait; n=0; fi
    done
    wait
}

{
    for s in $SEEDS; do echo "q_plain $s plain_$s.log"; done
    for s in $R_SEEDS; do echo "q_plain $s r_$s.log"; done
} > "$WORK/queue"
first=$(awk 'NR==1 { print $2 }' "$WORK/queue")
echo "q_CAL_INVERT $first nc_inv.log" >> "$WORK/queue"
echo "q_SRC_SWAP $first nc_swap.log" >> "$WORK/queue"
runq < "$WORK/queue"

nseed=0; p2=0; mc=0; mo=0; m_s33=0; m_tail=0; m_gate=0
for s in $SEEDS; do
    nseed=$((nseed + 1))
    log="$WORK/plain_$s.log"
    cat "$log"
    grep -q '^\[st-qalloc\] ALL PASS' "$log" || { echo "[st-qalloc] seed $s RED"; FAIL=1; }
    grep -q '^\[st-qalloc\] value P2 1' "$log" && p2=$((p2 + 1))
    line=$(grep -a '^\[st-qalloc\] value C1 ' "$log")
    if [ -z "$line" ]; then echo "[st-qalloc] FAIL C1 seed $s printed no miss line"; FAIL=1; continue; fi
    c=$(echo "$line" | sed -n 's/.*cal miss \([01]\).*/\1/p'); o=$(echo "$line" | sed -n 's/.*omega_el miss \([01]\).*/\1/p')
    mc=$((mc + ${c:-1})); mo=$((mo + ${o:-0}))
    for x in S33 TAIL GATE; do
        l=$(grep -a "^\[st-qalloc\] value SRC $x damage " "$log")
        if [ -z "$l" ]; then echo "[st-qalloc] FAIL SRC seed $s printed no $x line"; FAIL=1; continue; fi
        k=$(echo "$l" | sed -n 's/.* miss \([01]\) .*/\1/p')
        case "$x" in
            S33)  m_s33=$((m_s33 + ${k:-1})) ;;
            TAIL) m_tail=$((m_tail + ${k:-1})) ;;
            GATE) m_gate=$((m_gate + ${k:-1})) ;;
        esac
    done
done
need=$(( (nseed * 6 + 7) / 8 ))
if [ "$p2" -ge "$need" ]; then
    echo "[st-qalloc] PASS P2 the largest omega is worse than AUTO on $p2/$nseed seeds (need $need)"
else
    echo "[st-qalloc] FAIL P2 the largest omega is worse than AUTO on $p2/$nseed seeds (need $need)"; FAIL=1
fi
if [ "$mo" = 0 ]; then
    echo "[st-qalloc] FAIL C1 cannot judge: omega_el has no clear miss on these seeds (CAL $mc)"; FAIL=1
elif [ "$mc" -le 2 ] && [ "$mc" -lt "$mo" ]; then
    echo "[st-qalloc] PASS C1 clear misses CAL $mc vs omega_el $mo"
else
    echo "[st-qalloc] FAIL C1 clear misses CAL $mc vs omega_el $mo (need CAL <= 2 and fewer)"; FAIL=1
fi

# H (a reading fixed in §3, not a gate)
echo "[st-qalloc] value H clear misses: CAL $mc omega_el $mo S33 $m_s33 TAIL $m_tail GATE $m_gate (of $nseed)"
if [ "$mo" = 0 ]; then
    h="cannot decide (omega_el has no clear miss)"
elif [ "$m_s33" -le $((mc + 1)) ] && [ "$m_tail" -ge "$mo" ]; then
    h="close text is needed"
elif [ "$m_tail" -le $((mc + 1)) ] && [ "$m_tail" -lt "$mo" ]; then
    h="any unseen text will do"
else
    h="cannot decide"
fi
echo "[st-qalloc] value H reading: $h"

grep -a 'cal damage\|bpb damage' "$WORK/nc_inv.log"
if grep -aq '^\[st-qalloc\] FAIL P1 ' "$WORK/nc_inv.log"; then
    echo "[st-qalloc] PASS negative control CAL_INVERT turns P1 red"
else
    echo "[st-qalloc] FAIL negative control CAL_INVERT did not turn P1 red"; FAIL=1
fi

num() { grep -a "^\[st-qalloc\] value SRC $1 damage " "$2" | sed 's/.* damage //'; }
sw_t=$(num TAIL "$WORK/nc_swap.log"); sw_s=$(num S33 "$WORK/nc_swap.log")
pl_t=$(num TAIL "$WORK/plain_$first.log"); pl_s=$(num S33 "$WORK/plain_$first.log")
echo "[st-qalloc] SRC_SWAP seed $first: TAIL '$sw_t' S33 '$sw_s'; plain TAIL '$pl_t' S33 '$pl_s'"
if [ -n "$sw_t" ] && [ "$sw_t" = "$sw_s" ] && [ -n "$pl_t" ] && [ "$pl_t" != "$pl_s" ]; then
    echo "[st-qalloc] PASS negative control SRC_SWAP: the TAIL line equals S33 when fed S33, differs in plain"
else
    echo "[st-qalloc] FAIL negative control SRC_SWAP (the TAIL line does not read its own windows)"; FAIL=1
fi

if [ -n "$R_SEEDS" ]; then
    : > "$WORK/r.out"
    for s in $R_SEEDS; do
        grep -aE '^\[st-qalloc\] (cal damage|value C1 )' "$WORK/r_$s.log" >> "$WORK/r.out"
    done
    if diff "$HERE/qalloc_cal_expect.txt" "$WORK/r.out"; then
        echo "[st-qalloc] PASS R seeds $R_SEEDS: $(wc -l < "$WORK/r.out") lines equal the stage 8 record"
    else
        echo "[st-qalloc] FAIL R seeds $R_SEEDS differ from the stage 8 record"; FAIL=1
    fi
fi

if [ "$FAIL" = 0 ]; then echo "[st-qalloc] CERT ALL PASS"; exit 0; fi
echo "[st-qalloc] CERT RED"; exit 1
