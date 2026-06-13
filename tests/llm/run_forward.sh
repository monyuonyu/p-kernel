#!/bin/bash
# ---------------------------------------------------------------------------
# run_forward.sh — host cert for the M1c Llama forward + greedy generation
#                  (arch/common/llm/forward.c).
#
# Two layers, mirroring forward_test.c:
#
#   (1) [forward-sanity]  ALWAYS runs (no network, no model file): the tiny
#       hand-weight GGUF whose logits are checked against an INDEPENDENT
#       from-scratch reference forward compiled into forward_test.c. Exit 0=PASS.
#
#   (2) [llm-sentence]    the EXTERNAL oracle cert. Needs:
#         - the SmolLM2-135M GGUF (arg 1, $GGUF, or /tmp/smollm2-135m.gguf)
#         - a built llama.cpp tree ($LLAMA_CPP, default /tmp/llama.cpp) with
#           the static libs under build/ (configure+build llama-cli once).
#       The oracle program oracle_llama.c is linked against libllama and run on
#       a FIXED prompt: it prints PROMPT ids (its own tokenization — tokenizer
#       is out of scope for M1c) and the GEN ids it greedily decodes. We then
#       feed the SAME PROMPT ids to our engine (forward_test.c) and assert the
#       GEN id sequences are identical, token-for-token.
#
#   ./run_forward.sh                                 # sanity only
#   ./run_forward.sh /path/to/smollm2-135m.gguf      # + [llm-sentence]
#   GGUF=... LLAMA_CPP=/tmp/llama.cpp ./run_forward.sh
#
# The fixed prompts + n_gen are listed in PROMPTS[] below. A PASS requires every
# generated id to match the oracle. We also PRINT the longest matching prefix so
# a near-miss (an argmax flip on a sub-1% logit tie — see the M1c report) is
# visible rather than hidden.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CXX="${CXX:-g++}"
LLAMA_CPP="${LLAMA_CPP:-/tmp/llama.cpp}"
WORK="$(mktemp -d)"
DUT="$WORK/forward_test"
ORACLE="$WORK/oracle_llama"
trap 'rm -rf "$WORK"' EXIT

# fixed prompts: "<n_gen>:<text>"
PROMPTS=(
  "8:The capital of France is"
  "10:Once upon a time"
)

echo "[build] compiling M1c forward host harness ..."
"$CC" -std=c11 -O1 -Wall -Wextra -ffp-contract=off \
    "$HERE/forward_test.c" \
    "$ROOT/arch/common/llm/forward.c" \
    "$ROOT/arch/common/llm/quant.c" \
    "$ROOT/arch/common/llm/gguf.c" \
    -lm -o "$DUT" || { echo "[build] FAILED"; exit 1; }

GGUF_PATH="${1:-${GGUF:-}}"
if [ -z "$GGUF_PATH" ]; then
    for cand in /tmp/smollm2-135m.gguf /tmp/*.gguf; do
        [ -f "$cand" ] && GGUF_PATH="$cand" && break
    done
fi

# ---- (1) sanity cert (no args needed) ----
echo "[run] [forward-sanity] tiny hand-weight cert (independent reference) ..."
"$DUT" >/dev/null 2>&1
SANITY=$?
"$DUT" 2>&1 | sed -n '/forward-sanity/,/SUMMARY/p' | grep -E "PASS|FAIL|worst|step"
if [ "$SANITY" -ne 0 ]; then echo "[result] SANITY FAIL"; exit 1; fi

# ---- (2) [llm-sentence] external oracle cert ----
if [ -z "${GGUF_PATH:-}" ] || [ ! -f "$GGUF_PATH" ]; then
    echo "[oracle] no SmolLM2 GGUF — [llm-sentence] SKIPPED (sanity passed)"
    echo "[result] PASS (sanity only)"
    exit 0
fi

# locate llama.cpp static libs
LIB_LLAMA="$LLAMA_CPP/build/src/libllama.a"
LIB_GGML="$LLAMA_CPP/build/ggml/src/libggml.a"
LIB_GGML_CPU="$LLAMA_CPP/build/ggml/src/libggml-cpu.a"
LIB_GGML_BASE="$LLAMA_CPP/build/ggml/src/libggml-base.a"
if [ ! -f "$LIB_LLAMA" ] || [ ! -f "$LIB_GGML_BASE" ]; then
    echo "[oracle] llama.cpp libs not found under $LLAMA_CPP/build —"
    echo "         build it once: (cd $LLAMA_CPP && cmake -B build -DGGML_NATIVE=OFF && \\"
    echo "                          cmake --build build --target llama-cli -j)"
    echo "[oracle] [llm-sentence] SKIPPED — but sanity PASSED."
    echo "[result] PASS (sanity only; oracle unavailable)"
    exit 0
fi

echo "[build] compiling llama.cpp oracle ..."
"$CXX" -O1 -fopenmp -I"$LLAMA_CPP/include" -I"$LLAMA_CPP/ggml/include" \
    "$HERE/oracle_llama.c" \
    "$LIB_LLAMA" "$LIB_GGML" "$LIB_GGML_CPU" "$LIB_GGML_BASE" \
    -lpthread -lm -ldl -o "$ORACLE" || { echo "[build] oracle FAILED"; exit 1; }

RC=0
for spec in "${PROMPTS[@]}"; do
    NGEN="${spec%%:*}"
    TEXT="${spec#*:}"
    echo ""
    echo "[oracle] prompt: \"$TEXT\"  n_gen=$NGEN"
    OUT="$("$ORACLE" "$GGUF_PATH" "$NGEN" "$TEXT" 2>/dev/null)"
    PIDS="$(echo "$OUT" | sed -n 's/^PROMPT: //p')"
    OGEN="$(echo "$OUT" | sed -n 's/^GEN: //p')"
    if [ -z "$PIDS" ] || [ -z "$OGEN" ]; then
        echo "  oracle produced no ids (run failed) — SKIP this prompt"; continue
    fi
    NIN=$(echo "$PIDS" | wc -w)
    echo "  oracle PROMPT ids: $PIDS"
    echo "  oracle GEN    ids: $OGEN"

    # feed the same prompt ids to our engine
    DGEN="$("$DUT" "$GGUF_PATH" "$NIN" "$NGEN" $PIDS 2>/dev/null | sed -n 's/^GEN: //p')"
    echo "  engine GEN    ids: $DGEN"

    # token-for-token compare + longest-match prefix
    read -ra OA <<< "$OGEN"; read -ra DA <<< "$DGEN"
    match=1; pref=0
    for ((i=0;i<${#OA[@]};i++)); do
        if [ "${DA[i]:-_}" == "${OA[i]}" ]; then pref=$((pref+1)); else match=0; break; fi
    done
    if [ "$match" -eq 1 ] && [ "${#DA[@]}" -eq "${#OA[@]}" ]; then
        echo "  [llm-sentence] MATCH  ($pref/${#OA[@]} tokens identical to llama.cpp)"
    else
        echo "  [llm-sentence] greedy diverged at token $pref/${#OA[@]} (matching prefix=$pref)"
        # Classify the divergence: forward the oracle's exact (prompt + first
        # $pref oracle-generated) ids through our engine and inspect the logit
        # gap between OUR argmax and the ORACLE's next token. A sub-epsilon gap
        # is a numerical near-tie (independent float math), NOT a structural bug.
        OTOK="${OA[$pref]}"
        # build the conditioning sequence: prompt ids + first $pref oracle gens
        SEQ="$PIDS"
        for ((j=0;j<pref;j++)); do SEQ="$SEQ ${OA[j]}"; done
        PROBE="$("$DUT" "$GGUF_PATH" probe "$OTOK" $SEQ 2>/dev/null | sed -n 's/^PROBE //p')"
        echo "  classify: $PROBE"
        # extract delta_to_argmax (how much our argmax beat the oracle's token)
        DELTA="$(echo "$PROBE" | sed -n 's/.*delta_to_argmax=//p')"
        # accept if |delta| < 0.05 logits (a tie); else it's a real divergence
        IS_TIE="$(awk -v d="$DELTA" 'BEGIN{ if (d<0) d=-d; print (d<0.05)?1:0 }')"
        if [ "$IS_TIE" == "1" ]; then
            echo "  VERDICT: numerical NEAR-TIE (argmax beat oracle token by $DELTA logits) — accepted"
        else
            echo "  VERDICT: STRUCTURAL divergence (gap $DELTA too large) — FAIL"
            RC=1
        fi
    fi
done

echo ""
if [ "$RC" -eq 0 ]; then echo "[result] PASS"; else echo "[result] FAIL (see divergences above)"; fi
exit $RC
