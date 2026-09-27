#!/bin/bash
# ---------------------------------------------------------------------------
# run_st_merge.sh — host cert for the smarter merge, stage 1
#   (st_merge_barrier / st_merge_guarded in arch/common/llm/student.c;
#    docs/architecture/30-module/research/brain-merge.md §3).
#
# Builds student_merge_test.c twice and requires:
#   plain                 -> exit 0 (gates A..D PASS)
#   -DST_MERGE_PLAIN_ONLY -> exit 1 (the guard may only take the plain mean,
#                            which is the [ss3-blob-merge] failure: C RED)
#
#   ./run_st_merge.sh
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

SRC_TEST="$HERE/student_merge_test.c"
SRC_STU="$ROOT/arch/common/llm/student.c"

rc_all=0
for v in plain PLAIN_ONLY; do
    def=""
    [ "$v" != plain ] && def="-DST_MERGE_$v"
    echo "[build] $v $def"
    if ! $CC $CFLAGS $def "$SRC_TEST" "$SRC_STU" -o "$WORK/m_$v"; then
        echo "[build] $v FAILED"; rc_all=1; continue
    fi
    "$WORK/m_$v"
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

if [ "$rc_all" -eq 0 ]; then echo "[st-merge] PASS"; else echo "[st-merge] FAIL"; fi
exit "$rc_all"
