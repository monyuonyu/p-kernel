#!/bin/bash
# ---------------------------------------------------------------------------
# run_tokenizer.sh — host cert for the M1d BPE tokenizer
#                    (arch/common/llm/tokenizer.c).
#
# Three layers, mirroring tokenizer_test.c:
#
#   (1) [tok-sanity]    ALWAYS runs (no network, no model file): a tiny
#       hand-built byte-level BPE (vocab+merges) whose encode()/decode() are
#       checked against KNOWN ids inside tokenizer_test.c. Exit 0 = PASS.
#
#   (2) [tok-encode] + [tok-roundtrip]   need the SmolLM2-135M GGUF
#       (arg 1, $GGUF, or /tmp/smollm2-135m.gguf). The DUT prints, for a FIXED
#       set of strings (ASCII / punctuation / multi-byte UTF-8 / leading &
#       trailing spaces / newlines / digits / emoji), "ENC[i]: <ids>" and
#       "RT[i]: OK/FAIL". If a built llama.cpp tree ($LLAMA_CPP, default
#       /tmp/llama.cpp) is present, we build the oracle (oracle_tok.c — same
#       fixed strings via llama_tokenize) and diff ENC ids token-for-token
#       ([tok-encode]). [tok-roundtrip] is the DUT's own byte-exact
#       decode(encode(s))==s verdict and needs no oracle.
#
#   (3) END-TO-END (cheap, if the GGUF is present + the M1c harness builds):
#       encode("The capital of France is") and confirm the ids equal the M1c
#       cert prompt ids (504 3575 282 4649 314) — text -> ids -> teacher.
#
#   ./run_tokenizer.sh                              # sanity only
#   ./run_tokenizer.sh /path/to/smollm2-135m.gguf   # + encode/roundtrip/e2e
#   GGUF=... LLAMA_CPP=/tmp/llama.cpp ./run_tokenizer.sh
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CXX="${CXX:-g++}"
LLAMA_CPP="${LLAMA_CPP:-/tmp/llama.cpp}"
WORK="$(mktemp -d)"
DUT="$WORK/tokenizer_test"
ORACLE="$WORK/oracle_tok"
FWD="$WORK/forward_test"
trap 'rm -rf "$WORK"' EXIT

echo "[build] compiling M1d tokenizer host harness ..."
"$CC" -std=c11 -O1 -Wall -Wextra -ffp-contract=off \
    "$HERE/tokenizer_test.c" \
    "$ROOT/arch/common/llm/tokenizer.c" \
    "$ROOT/arch/common/llm/gguf.c" \
    -o "$DUT" || { echo "[build] FAILED"; exit 1; }

GGUF_PATH="${1:-${GGUF:-}}"
if [ -z "$GGUF_PATH" ]; then
    for cand in /tmp/smollm2-135m.gguf /tmp/*.gguf; do
        [ -f "$cand" ] && GGUF_PATH="$cand" && break
    done
fi

# ---- (1) sanity cert (no args needed) ----
echo "[run] [tok-sanity] tiny hand-built BPE cert ..."
"$DUT" >/dev/null 2>&1
SANITY=$?
"$DUT" 2>&1 | sed -n '/tok-sanity/,/SUMMARY/p' | grep -E "PASS|FAIL|SUMMARY"
if [ "$SANITY" -ne 0 ]; then echo "[result] SANITY FAIL"; exit 1; fi

# ---- need a GGUF for the rest ----
if [ -z "${GGUF_PATH:-}" ] || [ ! -f "$GGUF_PATH" ]; then
    echo "[oracle] no SmolLM2 GGUF — encode/roundtrip/e2e SKIPPED (sanity passed)"
    echo "[result] PASS (sanity only)"
    exit 0
fi

# DUT real-model run: produces ENC[i]/RT[i] + the [tok-roundtrip] verdict.
DUT_OUT="$("$DUT" "$GGUF_PATH" 2>/dev/null)"
echo "$DUT_OUT" | grep -E "^  vocab=|^\[tok-roundtrip\]"
RT_LINE="$(echo "$DUT_OUT" | grep '^\[tok-roundtrip\]')"

RC=0
[ "${RT_LINE#*PASS}" = "$RT_LINE" ] && { echo "  [tok-roundtrip] FAILED"; RC=1; }

# ---- (2) [tok-encode] external oracle diff ----
LIB_LLAMA="$LLAMA_CPP/build/src/libllama.a"
LIB_GGML_BASE="$LLAMA_CPP/build/ggml/src/libggml-base.a"
if [ -f "$LIB_LLAMA" ] && [ -f "$LIB_GGML_BASE" ]; then
    echo "[build] compiling llama.cpp tokenizer oracle ..."
    if "$CXX" -O1 -fopenmp -I"$LLAMA_CPP/include" -I"$LLAMA_CPP/ggml/include" \
        "$HERE/oracle_tok.c" \
        "$LIB_LLAMA" "$LLAMA_CPP/build/ggml/src/libggml.a" \
        "$LLAMA_CPP/build/ggml/src/libggml-cpu.a" "$LIB_GGML_BASE" \
        -lpthread -lm -ldl -o "$ORACLE" 2>/dev/null; then
        ODIFF="$(diff <(echo "$DUT_OUT" | grep '^ENC') \
                      <("$ORACLE" "$GGUF_PATH" 2>/dev/null | grep '^ENC'))"
        if [ -z "$ODIFF" ]; then
            NSTR="$(echo "$DUT_OUT" | grep -c '^ENC')"
            echo "  [tok-encode] MATCH — all $NSTR strings identical to llama.cpp"
        else
            echo "  [tok-encode] MISMATCH vs llama.cpp:"; echo "$ODIFF"; RC=1
        fi
    else
        echo "  [tok-encode] oracle build failed — SKIPPED (roundtrip still checked)"
    fi
else
    echo "  [tok-encode] llama.cpp libs not found under $LLAMA_CPP/build — SKIPPED"
    echo "               (build once: cmake -B build -DGGML_NATIVE=OFF && \\"
    echo "                            cmake --build build --target llama-cli -j)"
fi

# ---- (3) end-to-end: encode -> M1c forward prompt ids ----
EXPECT="504 3575 282 4649 314"
GOT="$("$DUT" enc "$GGUF_PATH" "The capital of France is" 2>/dev/null | sed 's/^ENC: //')"
if [ "$GOT" = "$EXPECT" ]; then
    echo "  [tok-e2e] encode(\"The capital of France is\") = $GOT == M1c cert prompt ids"
    # If the M1c forward harness builds, actually push the ids through it.
    if "$CC" -std=c11 -O1 -Wall -Wextra -ffp-contract=off \
        "$HERE/forward_test.c" "$ROOT/arch/common/llm/forward.c" \
        "$ROOT/arch/common/llm/quant.c" "$ROOT/arch/common/llm/pk_parallel.c" \
        "$ROOT/arch/common/llm/gguf.c" \
        -lpthread -lm -o "$FWD" 2>/dev/null; then
        NIN=$(echo $GOT | wc -w)
        GEN="$("$FWD" "$GGUF_PATH" "$NIN" 4 $GOT 2>/dev/null | sed -n 's/^GEN: //p')"
        echo "  [tok-e2e] text -> ids -> M1c forward GEN: $GEN"
    fi
else
    echo "  [tok-e2e] MISMATCH: got '$GOT' expected '$EXPECT'"; RC=1
fi

echo ""
if [ "$RC" -eq 0 ]; then echo "[result] PASS"; else echo "[result] FAIL (see above)"; fi
exit $RC
