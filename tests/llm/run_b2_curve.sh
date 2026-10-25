#!/bin/bash
# ---------------------------------------------------------------------------
# run_b2_curve.sh — [b2-curve] (ROADMAP B2): the S -> M -> L curve of the
# student's bits per byte on the independent held text (tests/llm/b1_held.txt)
# over data size D and optimiser steps k, tests/llm/b2_curve_test.c.
# Points, pass lines and readings were fixed before any run:
# docs/architecture/30-module/research/b2-size-curve.md
#
#   ./run_b2_curve.sh            exit 0 = ALL PASS
#   B2_TIERS="S M L"             tiers to run (default all three; L is one arm)
#   B2_JOBS=3                    arms run side by side
#   B2_OUT=<dir>                 keep the per-arm logs there (default: temp)
#
#   arms: S and M x D 768/1536/3072 x seed 1/2 (k up to 1536), L D=3072 seed 1
#   (k up to 768), S D=3072 seed 1 again (G2), S D=3072 seed 1 with the held
#   text's first half mixed in (-DB2_LEAK, the negative control LEAK).
#   The judge is tests/llm/b2_curve_check.py.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
TIERS="${B2_TIERS:-S M L}"
JOBS="${B2_JOBS:-3}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
OUT="${B2_OUT:-$WORK/out}"
mkdir -p "$OUT"
SRC="$HERE/b2_curve_test.c $ROOT/arch/common/llm/held_indep.c $ROOT/arch/common/llm/cradle.c $ROOT/arch/common/llm/student.c"

echo "[build] b2-curve (held_indep.c + cradle.c + student.c) ..."
# shellcheck disable=SC2086
$CC $CFLAGS $SRC -lm -o "$WORK/b2" || { echo "[build] FAILED"; exit 1; }
# shellcheck disable=SC2086
$CC $CFLAGS -DB2_LEAK $SRC -lm -o "$WORK/b2_leak" || { echo "[build] FAILED"; exit 1; }

# one line per arm: <name> <binary> <tier> <D> <seed> <kmax>
: > "$WORK/arms"
for t in $TIERS; do
    case "$t" in
    S|M)
        n=0; [ "$t" = M ] && n=1
        for d in 768 1536 3072; do
            for s in 1 2; do echo "$t-$d-$s b2 $n $d $s 1536" >> "$WORK/arms"; done
        done
        if [ "$t" = S ]; then
            echo "S-3072-1-again b2 0 3072 1 1536" >> "$WORK/arms"
            echo "S-3072-1-leak b2_leak 0 3072 1 1536" >> "$WORK/arms"
        fi ;;
    L) echo "L-3072-1 b2 2 3072 1 768" >> "$WORK/arms" ;;
    *) echo "[b2-curve] unknown tier $t"; exit 2 ;;
    esac
done
# the slow arm first, so it is not the last one left running
sort -t' ' -k3,3r "$WORK/arms" > "$WORK/arms.sorted"
echo "[b2-curve] $(wc -l < "$WORK/arms.sorted") arms, $JOBS at a time, tiers: $TIERS"

# shellcheck disable=SC2016
xargs -P "$JOBS" -L 1 sh -c '"$0/$2" "$1" "$3" "$4" "$5" "$6" > "$7/$8.log" 2>&1; echo "[b2-curve] arm $8 rc=$? $(date -Is)"' \
    < <(awk -v w="$WORK" -v h="$HERE/b1_held.txt" -v o="$OUT" '{print w, h, $2, $3, $4, $5, $6, o, $1}' "$WORK/arms.sorted")

python3 "$HERE/b2_curve_check.py" --tiers "$TIERS" "$OUT"
