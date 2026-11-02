#!/bin/bash
# ---------------------------------------------------------------------------
# run_b4_math.sh - [b4-math] M1 to M4 of research/b4-numerics.md
# (tests/llm/student_math_test.c).
#   ./run_b4_math.sh             exit 0 = plain green, and the hash of x86_64
#                                equals aarch64's when a cross compiler is here
#   B4_STRIDE=17                 stride over the bit patterns (1 = all of them)
# The falsifiers of the design come with the implementation: they need the
# recipes they break.
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

if [ "$RC" -eq 0 ] && [ "$HRC" -eq 0 ]; then
    echo "[result] PASS  b4-math"
    exit 0
fi
echo "[result] FAIL  b4-math rc=$RC hash=$HRC"
exit 1
