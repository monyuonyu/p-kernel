#!/bin/bash
# ---------------------------------------------------------------------------
# run_b4_math.sh - [b4-math] M1 to M4 of research/b4-numerics.md
# (tests/llm/student_math_test.c).
#   ./run_b4_math.sh             exit 0 = plain green, and the hash of x86_64
#                                equals aarch64's when a cross compiler is here
#   B4_STRIDE=17                 stride over the bit patterns (1 = all of them)
#   B4_NC_STRIDE=251             the same for the falsifiers
# Falsifiers (each must turn red exactly the lines it names):
#   -DST_SQRT_OLD                the root as it was: m1-sqrt, m1-inf, m4-adam,
#                                m4-adam-t
#   -DST_SQRT_NO_INF             no case for the exponent 0xFF: m1-inf, and
#                                m1-edges (a NaN's bits read as a number)
#   -DST_MATH_SHORT              exp and log two terms short: m2-exp, m2-log,
#                                m4-adam and m4-adam-t (bias correction uses both)
#   -DST_ADAM_EPS=0.015625f      stage 1's eps (not the default): m4-adam, m4-adam-t
#   -DST_ADAM_B2=0.99f           b2 wrong: m4-adam-t only (t = 1 cancels it)
#   -DST_ADAM_B1=0.8f            b1 wrong: m4-adam-t only
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
STRIDE="${B4_STRIDE:-17}"

echo "[build] b4-math ..."
# shellcheck disable=SC2086
"$CC" $CFLAGS "$HERE/student_math_test.c" -lm -o "$WORK/m" || { echo "[build] FAILED"; exit 1; }
"$WORK/m" "$STRIDE"; RC=$?

H=$("$WORK/m" hash); echo "$H"
HRC=0
if command -v aarch64-linux-gnu-gcc > /dev/null && command -v qemu-aarch64-static > /dev/null; then
    # shellcheck disable=SC2086
    aarch64-linux-gnu-gcc $CFLAGS "$HERE/student_math_test.c" -lm -o "$WORK/ma" \
        || { echo "[build] aarch64 FAILED"; exit 1; }
    A=$(qemu-aarch64-static -L /usr/aarch64-linux-gnu "$WORK/ma" hash); echo "$A"
    if [ "$H" = "$A" ]; then
        echo "  PASS  [m3-hash] x86_64 and aarch64 give the same bits"
    else
        echo "  FAIL  [m3-hash] x86_64 and aarch64 differ"; HRC=1
    fi
else
    echo "  SKIP  [m3-hash] no aarch64 cross compiler or qemu-user here"
fi

NCRC=0
NCSTRIDE="${B4_NC_STRIDE:-251}"
nc() {  # nc <flag> <lines that must be red, sorted, space separated>
    # shellcheck disable=SC2086
    "$CC" $CFLAGS "$1" "$HERE/student_math_test.c" -lm -o "$WORK/n" \
        || { echo "  FAIL  [nc] $1 did not build"; NCRC=1; return; }
    "$WORK/n" "$NCSTRIDE" > "$WORK/n.out"; local rc=$?
    local got
    got=$(sed -n 's/^  FAIL  \[\([^]]*\)\].*/\1/p' "$WORK/n.out" | sort | tr '\n' ' ' | sed 's/ $//')
    if [ "$rc" -eq 1 ] && [ "$got" = "$2" ] && grep -q '^  PASS  ' "$WORK/n.out"; then
        echo "  PASS  [nc] $1 is red on: $got"
    else
        echo "  FAIL  [nc] $1 rc=$rc red on: [$got], want [$2]"; NCRC=1
    fi
}
nc -DST_SQRT_OLD "m1-inf m1-sqrt m4-adam m4-adam-t"
nc -DST_SQRT_NO_INF "m1-edges m1-inf"
nc -DST_MATH_SHORT "m2-exp m2-log m4-adam m4-adam-t"
nc -DST_ADAM_EPS=0.015625f "m4-adam m4-adam-t"
nc -DST_ADAM_B2=0.99f "m4-adam-t"
nc -DST_ADAM_B1=0.8f "m4-adam-t"

if [ "$RC" -eq 0 ] && [ "$HRC" -eq 0 ] && [ "$NCRC" -eq 0 ]; then
    echo "[result] PASS  b4-math"
    exit 0
fi
echo "[result] FAIL  b4-math rc=$RC hash=$HRC nc=$NCRC"
exit 1
