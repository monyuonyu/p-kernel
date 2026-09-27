#!/bin/bash
# ---------------------------------------------------------------------------
# run_tk_conform.sh — the contract suite v1 on the hosted Linux x86_64 build
#   (inbox #5; arch/common/tk_conform.c, shell verb `tkconf`).
#
# The suite checks the μT-Kernel 3.0 tk_* promises user space relies on
# (tasks, semaphores, time: normal paths, error codes, boundaries). It must
# print exactly EXPECT "[tkc] PASS" lines and 0 FAIL — a check that silently
# stopped running would lower the count and turn this red.
#
# Negative controls: the SAME suite on a kernel that breaks a promise must go
# RED. Each one patches ONE line of the vendored kernel in a scratch copy:
#   NC-SEMPOLL  semaphore.c: a TMO_POL wait succeeds with one unit too few
#   NC-NODELAY  task_sync.c: tk_dly_tsk returns at once for any delay < 100 s
# A negative control that stays green fails this script.
#
#   tests/host/run_tk_conform.sh            (x86_64 host)
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
EXPECT=41
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
rc_all=0

drive() {  # $1 = tree root -> suite output on stdout
    printf 'tkconf\nexit\n' | PKERNEL_GALAXY=0 timeout 120 "$1/boot/linux_x86_64/p-kernel" 2>&1 \
        | tr -d '\r' | grep -a '^\[tkc\]'
}

verdict() {  # $1 = log -> "PASS" if EXPECT passes and 0 fails
    local p f
    p=$(grep -c '^\[tkc\] PASS ' "$1")
    f=$(grep -c '^\[tkc\] FAIL ' "$1")
    echo "  pass=$p fail=$f (expect $EXPECT/0)"
    [ "$p" -eq "$EXPECT" ] && [ "$f" -eq 0 ]
}

echo "[build] plain"
if ! make -C "$ROOT/boot/linux_x86_64" -j4 > "$WORK/build-plain.log" 2>&1; then
    tail -20 "$WORK/build-plain.log"; echo "[tk-conform] FAIL (build)"; exit 1
fi
drive "$ROOT" > "$WORK/plain.log"
cat "$WORK/plain.log"
if verdict "$WORK/plain.log"; then echo "[result] plain GREEN (expected)"
else echo "[result] plain RED (UNEXPECTED)"; rc_all=1; fi
echo ""

nc() {  # $1 = name, $2 = file under kernel/mtkernel3/kernel/tkernel, $3 = old, $4 = new
    local d="$WORK/nc-$1"
    mkdir -p "$d"
    cp -a "$ROOT/arch" "$ROOT/boot" "$ROOT/kernel" "$ROOT/include" "$ROOT/tools" "$d/"
    make -C "$d/boot/linux_x86_64" clean > /dev/null 2>&1
    python3 - "$d/kernel/mtkernel3/kernel/tkernel/$2" "$3" "$4" <<'EOF'
import sys
p, old, new = sys.argv[1:4]
s = open(p).read()
assert s.count(old) == 1, ('anchor not found once', old)
open(p, 'w').write(s.replace(old, new))
EOF
    [ $? -eq 0 ] || { echo "[result] NC $1: anchor missing (FAIL)"; rc_all=1; return; }
    echo "[build] NC $1"
    if ! make -C "$d/boot/linux_x86_64" -j4 > "$WORK/build-$1.log" 2>&1; then
        tail -20 "$WORK/build-$1.log"; echo "[result] NC $1 build FAILED"; rc_all=1; return
    fi
    drive "$d" > "$WORK/$1.log"
    grep '^\[tkc\] FAIL\|PASS /' "$WORK/$1.log"
    if verdict "$WORK/$1.log"; then echo "[result] NC $1 GREEN (UNEXPECTED: the suite missed a broken promise)"; rc_all=1
    else echo "[result] NC $1 RED (expected)"; fi
    echo ""
}
nc SEMPOLL semaphore.c '&& semcb->semcnt >= cnt ) {' '&& semcb->semcnt + 1 >= cnt ) {'
nc NODELAY task_sync.c '	if ( dlytim > 0 ) {' '	if ( dlytim > 100000 ) {'

if [ "$rc_all" -eq 0 ]; then echo "[tk-conform] PASS"; else echo "[tk-conform] FAIL"; fi
exit "$rc_all"
