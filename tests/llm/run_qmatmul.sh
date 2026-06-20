#!/bin/bash
# ---------------------------------------------------------------------------
# run_qmatmul.sh — host cert for the M1b quantized matmul
#                  (arch/common/llm/quant.c).
#
# Always runs the SYNTHETIC cert (self-contained, no network, no model file):
# hand-built Q8_0 + Q4_0 blocks with known scale/quants, checked against an
# INDEPENDENT in-file reference (full-dequant-then-textbook-matmul + a 2nd
# fp16 decode). Exit 0 = PASS.
#
# If a real GGUF is given (arg 1, or $GGUF, or auto-detected in /tmp), it ALSO
# runs the real-tensor matmul, times it, dumps y[], and — if python3 is present
# — diffs that y against the fully INDEPENDENT oracle (qmatmul_oracle.py), which
# re-parses the SAME GGUF from scratch and recomputes y in pure Python.
#
#   ./run_qmatmul.sh                      # synthetic cert only
#   ./run_qmatmul.sh /path/to/model.gguf  # + real-tensor-vs-python-oracle
#   GGUF=/tmp/smollm2-135m.gguf ./run_qmatmul.sh
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
WORK="$(mktemp -d)"
BIN="$WORK/qmatmul_test"
DUMP="$WORK/dut.txt"
trap 'rm -rf "$WORK"' EXIT

echo "[build] compiling M1b qmatmul host harness ..."
"$CC" -std=c11 -O1 -Wall -Wextra -ffp-contract=off \
    "$HERE/qmatmul_test.c" \
    "$ROOT/arch/common/llm/quant.c" \
    "$ROOT/arch/common/llm/pk_parallel.c" \
    "$ROOT/arch/common/llm/gguf.c" \
    -lpthread -o "$BIN" || { echo "[build] FAILED"; exit 1; }

GGUF_PATH="${1:-${GGUF:-}}"
if [ -z "$GGUF_PATH" ]; then
    for cand in /tmp/smollm2-135m.gguf /tmp/*.gguf; do
        [ -f "$cand" ] && GGUF_PATH="$cand" && break
    done
fi

RC=0
if [ -n "${GGUF_PATH:-}" ] && [ -f "$GGUF_PATH" ]; then
    echo "[run] real GGUF: $GGUF_PATH"
    "$BIN" "$GGUF_PATH" "$DUMP" || RC=1

    if command -v python3 >/dev/null 2>&1; then
        echo "[oracle] independent python recompute + diff ..."
        python3 "$HERE/qmatmul_oracle.py" "$GGUF_PATH" "$DUMP" || RC=1
    else
        echo "[oracle] python3 absent — skipped real-tensor independent diff"
        echo "         (synthetic in-C independent reference still ran)"
    fi
else
    echo "[run] no real GGUF supplied — synthetic cert only"
    "$BIN" || RC=1
fi

if [ "$RC" -eq 0 ]; then
    echo "[result] PASS"
else
    echo "[result] FAIL"
fi
exit $RC
