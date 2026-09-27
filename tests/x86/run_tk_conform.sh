#!/bin/bash
# ---------------------------------------------------------------------------
# run_tk_conform.sh — the contract suite v1 on bare-metal x86 (QEMU)
#   (inbox #5; arch/common/tk_conform.c; the hosted twin is
#    tests/host/run_tk_conform.sh).
#
# boot/x86 links tk_conform.c only when EXTRA_CFLAGS has -DPK_TKCONF; then
# usermain() runs the suite right after kernel_selftest() and prints the
# "[tkc]" lines on the serial port. The DEFAULT build does not contain it, so
# the crown (.text of the default kernel) does not move.
#
# Every arm is built in a scratch copy of the tree. Plain must print exactly
# EXPECT PASS lines and 0 FAIL. The same three kernel-breaking negative
# controls as the hosted script must go RED.
#
#   tests/x86/run_tk_conform.sh     (needs gcc -m32 toolchain as boot/x86 and
#                                    qemu-system-x86_64)
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
EXPECT=41
BOOT_TIMEOUT="${TKC_BOOT_TIMEOUT:-60}"
QEMU="${QEMU:-qemu-system-x86_64}"
command -v "$QEMU" >/dev/null 2>&1 || { echo "[tk-conform-x86] FAIL: $QEMU not found"; exit 1; }
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
rc_all=0

arm() {  # $1 = name, $2 = kernel file ('' for plain), $3 = old, $4 = new, $5 = expect GREEN|RED
    local d="$WORK/$1" log="$WORK/$1.serial" p f v
    mkdir -p "$d"
    cp -a "$ROOT/arch" "$ROOT/boot" "$ROOT/kernel" "$ROOT/include" "$ROOT/tools" "$ROOT/relay" "$d/"
    if [ -n "$2" ]; then
        python3 - "$d/kernel/mtkernel3/kernel/tkernel/$2" "$3" "$4" <<'EOF'
import sys
p, old, new = sys.argv[1:4]
s = open(p).read()
assert s.count(old) == 1, ('anchor not found once', old)
open(p, 'w').write(s.replace(old, new))
EOF
        [ $? -eq 0 ] || { echo "[result] $1: anchor missing (FAIL)"; rc_all=1; return; }
    fi
    ( cd "$d/boot/x86" && make clean ) > "$WORK/$1.build" 2>&1
    if ! ( cd "$d/boot/x86" && make EXTRA_CFLAGS=-DPK_TKCONF bootloader.bin ) >> "$WORK/$1.build" 2>&1; then
        tail -20 "$WORK/$1.build"; echo "[result] $1 build FAILED"; rc_all=1; return
    fi
    : > "$log"
    ( cd "$d/boot/x86" && timeout "$BOOT_TIMEOUT" "$QEMU" -m 256 -kernel bootloader.bin \
        -serial file:"$log" -cpu qemu64 -display none -no-reboot ) > "$WORK/$1.qemu" 2>&1
    tr -d '\r' < "$log" | grep -a '^\[tkc\] FAIL\|^\[tkc\] [0-9]* PASS /\|^\[tkc\] contract'
    p=$(tr -d '\r' < "$log" | grep -ac '^\[tkc\] PASS ')
    f=$(tr -d '\r' < "$log" | grep -ac '^\[tkc\] FAIL ')
    echo "  pass=$p fail=$f (expect $EXPECT/0 for GREEN)"
    if [ "$p" -eq "$EXPECT" ] && [ "$f" -eq 0 ]; then v=GREEN; else v=RED; fi
    if [ "$v" = "$5" ]; then echo "[result] $1 $v (expected)"
    else echo "[result] $1 $v (UNEXPECTED)"; rc_all=1; fi
    echo ""
}
arm plain '' '' '' GREEN
arm NC-SEMPOLL semaphore.c '&& semcb->semcnt >= cnt ) {' '&& semcb->semcnt + 1 >= cnt ) {' RED
arm NC-WAIPAR semaphore.c $'\tCHECK_PAR(cnt > 0);\n\tCHECK_TMOUT(tmout);' $'\tCHECK_TMOUT(tmout);' RED
arm NC-HALFDELAY task_sync.c 'knl_make_wait_reltim(dlytim, TA_NULL);' 'knl_make_wait_reltim(dlytim / 2, TA_NULL);' RED

if [ "$rc_all" -eq 0 ]; then echo "[tk-conform-x86] PASS"; else echo "[tk-conform-x86] FAIL"; fi
exit "$rc_all"
