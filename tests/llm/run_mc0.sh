#!/bin/bash
# ---------------------------------------------------------------------------
# run_mc0.sh — MC-0 cert for the deterministic row-partitioned parallel matmul
#              (arch/common/llm/pk_parallel.c + re-expressed qz_matmul_q8_0).
#
# Self-contained (no GGUF, no network). Builds the in-process harness and runs:
#   [par-matmul-equiv]     memcmp==0 + FNV hash match for NW in {1,2,4,8},
#                          incl. NW that does NOT divide out (ragged remainder).
#   [par-matmul-falsifier] a reassociating variant MUST FAIL memcmp (teeth).
#   [mc0-idle]             the worker pool BLOCKS between matmuls (no spin).
#
# Build: -O1 -ffp-contract=off (one mind, one math). Link: -lpthread.
# Exit 0 = PASS.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
WORK="$(mktemp -d)"
BIN="$WORK/mc0_test"
trap 'rm -rf "$WORK"' EXIT

echo "[build] compiling MC-0 host harness ..."
"$CC" -std=c11 -O1 -Wall -Wextra -ffp-contract=off \
    -I"$ROOT/arch/common/llm" \
    "$HERE/mc0_test.c" \
    "$ROOT/arch/common/llm/quant.c" \
    "$ROOT/arch/common/llm/pk_parallel.c" \
    -lpthread \
    -o "$BIN" || { echo "[build] FAILED"; exit 1; }

echo "[run] MC-0 in-process cert ..."
"$BIN"
RC=$?

if [ "$RC" -eq 0 ]; then
    echo "[result] PASS"
else
    echo "[result] FAIL"
fi
exit $RC
