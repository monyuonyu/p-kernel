#!/bin/bash
# ---------------------------------------------------------------------------
# run.sh — host cert for the GGUF loader (arch/common/llm/gguf.c, M1a).
#
# Always runs the SYNTHETIC round-trip cert (self-contained, no network, no
# model file needed): build a spec-valid GGUF in memory, parse it back, assert
# exact equality of header / metadata KVs / tensor table. Exit 0 = PASS.
#
# If a real GGUF path is given (arg 1, or $GGUF env, or a file auto-detected in
# /tmp), it is ALSO dumped first so its config can be eyeballed / cross-checked
# against the model's HF config.json.
#
#   ./run.sh                         # synthetic cert only
#   ./run.sh /path/to/model.gguf     # real-file dump + synthetic cert
#   GGUF=/tmp/smollm2-135m.gguf ./run.sh
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
WORK="$(mktemp -d)"
BIN="$WORK/gguf_test"
trap 'rm -rf "$WORK"' EXIT

echo "[build] compiling GGUF host harness ..."
"$CC" -std=c11 -O1 -Wall -Wextra -ffp-contract=off \
    "$HERE/gguf_test.c" "$ROOT/arch/common/llm/gguf.c" \
    -o "$BIN" || { echo "[build] FAILED"; exit 1; }

GGUF_PATH="${1:-${GGUF:-}}"
if [ -z "$GGUF_PATH" ]; then
    # auto-detect a downloaded model for convenience (not required)
    for cand in /tmp/smollm2-135m.gguf /tmp/*.gguf; do
        [ -f "$cand" ] && GGUF_PATH="$cand" && break
    done
fi

if [ -n "${GGUF_PATH:-}" ] && [ -f "$GGUF_PATH" ]; then
    echo "[run] real GGUF: $GGUF_PATH"
    "$BIN" "$GGUF_PATH"
else
    echo "[run] no real GGUF supplied — synthetic cert only"
    "$BIN"
fi
