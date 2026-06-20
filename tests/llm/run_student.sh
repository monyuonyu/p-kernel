#!/bin/bash
# ---------------------------------------------------------------------------
# run_student.sh — host cert for the NS-1 Cradle baby
#                  (arch/common/llm/student.c, native-student.md §B.6/§B.7).
#
# Always runs the four self-contained certs (no network needed):
#   [baby-gradcheck]    analytic vs finite-diff gradients agree
#   [honest-baby]       fresh baby starts near chance (~ln(256) nats)
#   [distill-loss-drops] held-out next-byte loss drops after sleep rounds
#   [distill-grounded]  a scrambled-teacher control gains far less
# Exit 0 = all PASS.
#
# TEACHER FIXTURE (optional, sequence-level bridge, native-student.md §B.6(a)):
#   If a SmolLM2-135M GGUF is supplied (arg 1, $GGUF, or /tmp/smollm2-135m.gguf)
#   AND no fixture exists yet, this script HARVESTS one ONCE: it runs the
#   teacher to greedily generate text and writes the raw BYTES to
#   tests/llm/student_teacher.bytes. The cert then trains the baby on the
#   teacher's bytes. Without a GGUF the cert uses a built-in fallback byte
#   stream (clearly labelled) so CI still exercises the machinery.
#
#   ./run_student.sh                              # cert (fixture if present, else fallback)
#   ./run_student.sh /tmp/smollm2-135m.gguf       # harvest a fixture (once) + cert
#   GGUF=/tmp/smollm2-135m.gguf ./run_student.sh
#   HARVEST_TOKENS=512 ./run_student.sh /tmp/smollm2-135m.gguf   # longer harvest
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
WORK="$(mktemp -d)"
BIN="$WORK/student_test"
HARV="$WORK/harvest"
FIXTURE="$HERE/student_teacher.bytes"
trap 'rm -rf "$WORK"' EXIT

echo "[build] compiling NS-1 student cert ..."
"$CC" -std=c11 -O1 -Wall -Wextra -ffp-contract=off \
    "$HERE/student_test.c" "$ROOT/arch/common/llm/student.c" \
    -o "$BIN" || { echo "[build] FAILED"; exit 1; }

# ---- optional one-time teacher harvest ----
GGUF_PATH="${1:-${GGUF:-}}"
if [ -z "$GGUF_PATH" ]; then
    for cand in /tmp/smollm2-135m.gguf /tmp/*.gguf; do
        [ -f "$cand" ] && GGUF_PATH="$cand" && break
    done
fi
if [ -n "${GGUF_PATH:-}" ] && [ -f "$GGUF_PATH" ] && [ ! -f "$FIXTURE" ]; then
    echo "[harvest] building teacher harvester (forward.c + quant.c + gguf.c) ..."
    "$CC" -std=c11 -O1 -Wall -Wextra -ffp-contract=off \
        "$HERE/student_harvest.c" \
        "$ROOT/arch/common/llm/forward.c" \
        "$ROOT/arch/common/llm/quant.c" \
        "$ROOT/arch/common/llm/pk_parallel.c" \
        "$ROOT/arch/common/llm/gguf.c" \
        -lpthread -lm -o "$HARV" \
      && { echo "[harvest] running teacher (slow, ~250ms/token) ..."
           "$HARV" "$GGUF_PATH" "${HARVEST_TOKENS:-256}" "$FIXTURE" \
             && echo "[harvest] wrote $FIXTURE ($(wc -c < "$FIXTURE") bytes)" \
             || echo "[harvest] FAILED — cert will use the built-in fallback"; } \
      || echo "[harvest] build FAILED — cert will use the built-in fallback"
elif [ -f "$FIXTURE" ]; then
    echo "[harvest] reusing existing fixture $FIXTURE ($(wc -c < "$FIXTURE") bytes)"
else
    echo "[harvest] no GGUF — cert will use the built-in fallback byte stream"
fi

# ---- run cert ----
echo "[run] cert ..."
( cd "$ROOT" && "$BIN" --fixture "$FIXTURE" )
RC=$?
echo ""
if [ "$RC" -eq 0 ]; then echo "[result] PASS"; else echo "[result] FAIL"; fi
exit $RC
