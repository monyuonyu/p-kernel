#!/bin/bash
# ---------------------------------------------------------------------------
# run_st_quant.sh — host cert for the brain's self-quantization, stage 1
#   (st_quant_fake in arch/common/llm/student.c;
#    docs/architecture/30-module/research/brain-quantization.md §3).
#
# Builds student_quant_test.c three ways and requires:
#   plain               -> exit 0 (gates A..E all PASS)
#   -DST_QUANT_NOOP     -> exit 1 (copies w unchanged: gate C "changed>0" RED)
#   -DST_QUANT_BADSCALE -> exit 1 (s/4 clips: gate C bound RED)
# A negative control that goes GREEN fails this script: the gates must be able
# to see a broken quantizer, not just pass a working one.
#
# Thresholds (student_quant_test.c): EPS8 = 0.01 nats, DMG2 = 0.10 nats — see
# the research memo §3 for the measured values they were set against.
#
#   ./run_st_quant.sh
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

SRC_TEST="$HERE/student_quant_test.c"
SRC_STU="$ROOT/arch/common/llm/student.c"

rc_all=0
for v in plain NOOP BADSCALE; do
    def=""
    [ "$v" != plain ] && def="-DST_QUANT_$v"
    echo "[build] $v $def"
    if ! $CC $CFLAGS $def "$SRC_TEST" "$SRC_STU" -o "$WORK/q_$v"; then
        echo "[build] $v FAILED"; rc_all=1; continue
    fi
    "$WORK/q_$v"
    rc=$?
    if [ "$v" = plain ]; then
        if [ "$rc" -eq 0 ]; then echo "[result] plain GREEN (expected)"
        else echo "[result] plain RED rc=$rc (UNEXPECTED)"; rc_all=1; fi
    else
        if [ "$rc" -eq 1 ]; then echo "[result] $v RED (expected: negative control bites)"
        else echo "[result] $v rc=$rc (UNEXPECTED: negative control did not go red)"; rc_all=1; fi
    fi
    echo ""
done

if [ "$rc_all" -eq 0 ]; then echo "[st-quant] PASS"; else echo "[st-quant] FAIL"; fi
exit "$rc_all"
