#!/bin/bash
# ---------------------------------------------------------------------------
# run_b3_data.sh — [b3-data] (ROADMAP B3): does the student get better on the
# independent held text when it learns more unique teacher text?
# Points, pass lines and readings were fixed before any run:
# docs/architecture/30-module/research/b3-data.md
#
#   ./run_b3_data.sh <data dir>       exit 0 = ALL PASS
#   <data dir> holds train.txt and gen_held.txt (b3_prep.py) and
#   gen-report.txt (the [b3-teacher] and [b3-filter] lines). Not in CI: the
#   texts are made by the teacher (b3_gen.py) and are not kept in git.
#   B3_ARMS="S-1 ..."   arms to run (default all 7)
#   B3_JOBS=4           arms run side by side
#   B3_OUT=<dir>        keep the per-arm logs there (default: <data dir>/logs)
#
#   arms: S and M x seed 1/2 up to U=1.5 MB, S seed 1 again up to 96 KB (G2),
#   S-rep (the first 24 KB repeated up to 1.5 MB), S-leak (U=384 KB with the
#   held text's first half mixed in 4 times). Judge: b3_curve_check.py.
#   Time (B2's speeds): S about 50 min, M about 2.7 h per 1.5 MB arm.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
DATA="${1:?data dir with train.txt gen_held.txt gen-report.txt}"
DATA="$(cd "$DATA" && pwd)"
ARMS="${B3_ARMS:-M-1 M-2 S-1 S-2 S-rep S-leak S-1-again}"
JOBS="${B3_JOBS:-4}"
OUT="${B3_OUT:-$DATA/logs}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"
for f in train.txt gen_held.txt gen-report.txt; do
    [ -f "$DATA/$f" ] || { echo "[b3-data] FAIL $DATA/$f is missing"; exit 2; }
done
cp "$DATA/gen-report.txt" "$OUT/gen-report.txt"

echo "[build] b3-data (student.c) ..."
# shellcheck disable=SC2086
$CC $CFLAGS "$HERE/b3_curve_test.c" "$ROOT/arch/common/llm/student.c" -lm -o "$WORK/b3" \
    || { echo "[build] FAILED"; exit 1; }

# one line per arm: <name> <tier> <seed> <mode> <Umax>
: > "$WORK/arms"
for a in $ARMS; do
    case "$a" in
    S-1|S-2) echo "$a 0 ${a#S-} plain 1572864" ;;
    M-1|M-2) echo "$a 1 ${a#M-} plain 1572864" ;;
    S-1-again) echo "$a 0 1 plain 98304" ;;
    S-rep) echo "$a 0 1 rep 1572864" ;;
    S-leak) echo "$a 0 1 leak 393216" ;;
    *) echo "[b3-data] unknown arm $a"; exit 2 ;;
    esac >> "$WORK/arms"
done
echo "[b3-data] $(wc -l < "$WORK/arms") arms, $JOBS at a time: $ARMS"
ln -s "$HERE/b1_held.txt" "$WORK/held"
ln -s "$DATA/gen_held.txt" "$WORK/gen"
ln -s "$DATA/train.txt" "$WORK/train"

# shellcheck disable=SC2016
xargs -P "$JOBS" -L 1 sh -c '"$0" "$1/held" "$1/gen" "$1/train" "$3" "$4" "$5" "$6" > "$7/$2.log" 2>&1; echo "[b3-data] arm $2 rc=$? $(date -Is)"' \
    < <(awk -v b="$WORK/b3" -v w="$WORK" -v o="$OUT" '{print b, w, $1, $2, $3, $4, $5, o}' "$WORK/arms")

python3 "$HERE/b3_curve_check.py" "$OUT"
