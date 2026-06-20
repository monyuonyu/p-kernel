#!/bin/bash
# ---------------------------------------------------------------------------
# run_mc1.sh — MC-1 cert: the row-partitioned pool wired into the teacher
#              matmuls behind the out*in SIZE GATE + the measured speedup.
#
#   [par-matmul-equiv]    synthetic teacher-scale F32 + Q4_0 matmuls are
#                         byte-identical across NW{1,2,4,8} (memcmp + FNV hash),
#                         incl. ragged out. If the SmolLM2-135M GGUF is present
#                         a FULL teacher forward hash is also asserted identical
#                         across NW (one mind, one math). If absent -> deferred,
#                         stated honestly.
#   [par-matmul-gate]     the gate routes the small student (d=128) + R3 serial
#                         and the teacher ffn/head parallel.
#   [par-matmul-speedup]  wall-clock NW{1,2,4,8} on a teacher-scale matmul (+ the
#                         full teacher forward if the GGUF is present); the
#                         CROSSOVER sweep 2^12..2^22 (sets the gate constant).
#   [mc1-idle]            the pool blocks between forwards (no busy-spin).
#
# Build: -O1 -ffp-contract=off (one mind, one math). Link: -lpthread (+ -lm for
# the GGUF harness). Exit 0 = PASS.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
WORK="$(mktemp -d)"
BIN="$WORK/mc1_test"
HBIN="$WORK/mc1_hash"
trap 'rm -rf "$WORK"' EXIT

LLM="$ROOT/arch/common/llm"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -I$LLM"

echo "[build] compiling MC-1 self-contained harness (synthetic) ..."
$CC $CFLAGS "$HERE/mc1_test.c" "$LLM/quant.c" "$LLM/pk_parallel.c" \
    -lpthread -o "$BIN" || { echo "[build] FAILED"; exit 1; }

RC=0

echo ""
echo "########## [par-matmul-equiv] + [par-matmul-gate] + [mc1-idle] ##########"
"$BIN" equiv || RC=1
echo ""
"$BIN" gate  || RC=1
echo ""
"$BIN" idle  || RC=1

echo ""
echo "########## [par-matmul-speedup] headline (teacher-scale matmul) ##########"
"$BIN" speedup
echo ""
echo "########## [par-matmul-speedup] CROSSOVER SWEEP ##########"
"$BIN" sweep

# ---- full teacher forward equivalence (needs the GGUF) ----
GGUF_PATH="${1:-${GGUF:-}}"
if [ -z "$GGUF_PATH" ]; then
    for cand in /tmp/smollm2-135m.gguf /tmp/*.gguf; do
        [ -f "$cand" ] && GGUF_PATH="$cand" && break
    done
fi

echo ""
echo "########## [par-matmul-equiv] FULL TEACHER FORWARD across NW ##########"
if [ -z "${GGUF_PATH:-}" ] || [ ! -f "$GGUF_PATH" ]; then
    echo "[deferred] no SmolLM2-135M GGUF found (/tmp/smollm2-135m.gguf)."
    echo "           full-forward byte-identity DEFERRED to where the GGUF exists;"
    echo "           synthetic teacher-scale F32+Q4_0 equiv above stands in (PASS)."
else
    echo "[build] compiling MC-1 full-forward hash harness ..."
    $CC $CFLAGS "$HERE/mc1_hash.c" "$LLM/forward.c" "$LLM/quant.c" \
        "$LLM/pk_parallel.c" "$LLM/gguf.c" -lpthread -lm -o "$HBIN" \
        || { echo "[build] hash harness FAILED"; RC=1; }
    if [ -f "$HBIN" ]; then
        # llama.cpp ids for "The capital of France is" (BOS=1 + 5 pieces); 6 steps.
        PROMPT="1 504 5538 282 7138 314"
        NIN=6; NSTEPS=6
        echo "[run] full teacher forward, prompt ids: $PROMPT  steps=$NSTEPS"
        GOLD=""
        for NW in 1 2 4 8; do
            OUT="$(PKERNEL_MATMUL_THREADS=$NW "$HBIN" "$GGUF_PATH" $NIN $NSTEPS $PROMPT 2>/dev/null)"
            H="$(echo "$OUT" | sed -n 's/^HASH //p')"
            MS="$(echo "$OUT" | sed -n 's/^FWDMS //p')"
            AX="$(echo "$OUT" | sed -n 's/.*ARGMAX //p')"
            [ -z "$GOLD" ] && GOLD="$H"
            if [ "$H" == "$GOLD" ]; then VERD="IDENTICAL"; else VERD="DIVERGED -> FAIL"; RC=1; fi
            printf "  NW=%d  hash=%s  %.2s ms/fwd=%s  argmax=%s  %s\n" \
                   "$NW" "$H" "" "$MS" "$AX" "$VERD"
        done
        if [ "$RC" -eq 0 ]; then
            echo "  [par-matmul-equiv] FULL TEACHER FORWARD byte-identical across NW{1,2,4,8}."
        else
            echo "  [par-matmul-equiv] FULL TEACHER FORWARD DIVERGED -> one-mind broken."
        fi
    fi
fi

echo ""
if [ "$RC" -eq 0 ]; then echo "[result] MC-1 PASS"; else echo "[result] MC-1 FAIL"; fi
exit $RC
