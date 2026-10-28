#!/bin/bash
# ---------------------------------------------------------------------------
# run_b3_teacher.sh — [b3-teacher] (ROADMAP B3): is the teacher that makes the
# B3 texts through PyTorch (b3_gen.py) the same teacher as the kernel's own
# forward (arch/common/llm/forward.c)? Both read the same GGUF; the first 256
# token ids of tests/llm/b1_cal.txt go through both and the top-1 next token is
# compared per position. Pass line (research/b3-data.md section 2): >= 95 %.
# Not in CI (needs the model file and a Python with torch + transformers).
#
#   B3_PY=<python with torch> ./run_b3_teacher.sh <dir with the GGUF> <gguf name>
#   prints "[b3-teacher] match=N of 256"                     exit 0 = >= 95 %
#   B3_TEACHER_NC=1   negative control: the own forward reads the ids moved by
#                     one token (its context is not the one PyTorch saw) -> red
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
PY="${B3_PY:-python3}"
MD="${1:?model dir}"
GG="${2:?gguf file name}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=c11 -O1 -Wall -Wextra -ffp-contract=off "$HERE/b3_teacher_top1.c" \
    "$ROOT/arch/common/llm/forward.c" "$ROOT/arch/common/llm/quant.c" \
    "$ROOT/arch/common/llm/pk_parallel.c" "$ROOT/arch/common/llm/gguf.c" \
    -lpthread -lm -o "$WORK/top1" || { echo "[build] FAILED"; exit 1; }
"$PY" -I "$HERE/b3_gen.py" --model-dir "$MD" --gguf "$GG" --encode "$HERE/b1_cal.txt" > "$WORK/ids.txt" 2>/dev/null \
    || { echo "[b3-teacher] encode FAILED"; exit 1; }
"$PY" -I "$HERE/b3_gen.py" --model-dir "$MD" --gguf "$GG" --top1 "$WORK/ids.txt" > "$WORK/torch.txt" 2>/dev/null \
    || { echo "[b3-teacher] PyTorch top-1 FAILED"; exit 1; }
OWN_IDS="$WORK/ids.txt"
if [ "${B3_TEACHER_NC:-0}" = 1 ]; then
    { echo 1; head -n 255 "$WORK/ids.txt"; } > "$WORK/ids-nc.txt"
    OWN_IDS="$WORK/ids-nc.txt"
    echo "[b3-teacher] NC: own forward reads the ids moved by one"
fi
"$WORK/top1" "$MD/$GG" "$OWN_IDS" > "$WORK/own.txt" || { echo "[b3-teacher] own forward FAILED"; exit 1; }
python3 - "$WORK/torch.txt" "$WORK/own.txt" <<'EOF'
import sys
a = [l.split() for l in open(sys.argv[1]) if l.strip()]
b = [l.split() for l in open(sys.argv[2]) if l.strip()]
n = min(len(a), len(b))
match = sum(1 for i in range(n) if a[i] == b[i])
print("[b3-teacher] positions torch %d own %d" % (len(a), len(b)))
print("[b3-teacher] match=%d of %d" % (match, n))
sys.exit(0 if n == 256 and match >= 0.95 * n else 1)
EOF
