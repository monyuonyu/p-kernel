#!/bin/bash
# ---------------------------------------------------------------------------
# run_diverse_proof.sh — cert for step ⑤ ("make the baby's babble richer").
#
# Proves the DIVERSE teacher corpus (harvested via the SS-1 sampler over many
# varied prompts — see student_harvest_diverse.c) yields a LESS REPETITIVE baby
# than the OLD small repetitive fixture. Distills two babies from the same fresh
# seed + geometry (one per corpus) and compares:
#   distinct-byte-trigram ratio (higher = richer)  -- must increase
#   held-out next-byte loss     (nats)             -- must not be meaningfully worse
#   longest byte run / max 4-gram repeats          -- reported (lower = better)
# plus a generation SAMPLE from each so the difference is visible.
#
# SELF-CONTAINED (no network, no GGUF, no committed *.bytes — those are
# .gitignored): the OLD corpus is built into student_diverse_proof.c, and the
# NEW corpus is EXTRACTED at run time from the TEACHER_FIXTURE C string literal
# now living in arch/common/llm/student_shell.c (the single source of truth).
#
# Re-harvesting the diverse corpus from scratch (slow, needs the GGUF) is a
# separate manual step — see student_harvest_diverse.c — and is NOT part of CI.
# Exit 0 = PASS (richer AND loss not worse).
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

SHELL_C="$ROOT/arch/common/llm/student_shell.c"
NEW="$WORK/diverse_teacher.bytes"

# ---- extract the TEACHER_FIXTURE literal from student_shell.c ----
# Pull the string-literal lines between `TEACHER_FIXTURE[] =` and the closing
# `;`, strip the surrounding quotes, and un-escape \" and \\ so the bytes match
# what the C compiler embeds.
echo "[extract] TEACHER_FIXTURE from student_shell.c -> NEW corpus ..."
awk '
  /TEACHER_FIXTURE\[\] *=/ { grab=1; next }
  grab {
    line=$0
    # isolate the quoted part(s) on this line
    # remove leading whitespace
    gsub(/^[ \t]+/, "", line)
    # does it end the literal?
    endlit = (line ~ /";[ \t]*$/)
    # strip trailing ; if present
    sub(/;[ \t]*$/, "", line)
    # strip the opening and closing quote of the chunk
    sub(/^"/, "", line)
    sub(/"[ \t]*$/, "", line)
    printf "%s", line
    if (endlit) exit
  }
' "$SHELL_C" \
  | sed -e 's/\\"/"/g' -e 's/\\\\/\\/g' \
  > "$NEW"

NBYTES=$(wc -c < "$NEW")
echo "[extract] NEW corpus = $NBYTES bytes"
if [ "$NBYTES" -lt 1000 ]; then
    echo "[extract] FAILED — extracted corpus too small; check the awk parser"
    exit 1
fi

echo "[build] diverse-baby proof ..."
$CC $CFLAGS "$HERE/student_diverse_proof.c" "$ROOT/arch/common/llm/student.c" \
    -o "$WORK/proof" || { echo "[build] FAILED"; exit 1; }

echo "[run] proof (built-in OLD vs extracted NEW) ..."
"$WORK/proof" "$NEW"
RC=$?
echo ""
if [ "$RC" -eq 0 ]; then echo "[result] PASS"; else echo "[result] FAIL"; fi
exit $RC
