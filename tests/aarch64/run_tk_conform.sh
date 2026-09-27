#!/bin/bash
# ---------------------------------------------------------------------------
# run_tk_conform.sh — the contract suite v1 on bare-metal AArch64 (QEMU virt)
#   (inbox #5; arch/common/tk_conform.c; twins: tests/host/run_tk_conform.sh,
#    tests/x86/run_tk_conform.sh).
#
# boot/aarch64 links tk_conform.c only when EXTRA_CFLAGS has -DPK_TKCONF; then
# usermain() runs the suite right after the banner and prints "[tkc]" lines on
# the UART. The DEFAULT build does not contain it, so the crown is unchanged.
#
# Every arm is built in a scratch copy of the tree. Plain must print exactly
# EXPECT PASS lines and 0 FAIL; the same three kernel-breaking negative
# controls as the other two scripts must go RED.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
EXPECT=62
BOOT_TIMEOUT="${TKC_BOOT_TIMEOUT:-60}"
QEMU="${QEMU:-qemu-system-aarch64}"
command -v "$QEMU" >/dev/null 2>&1 || { echo "[tk-conform-a64] FAIL: $QEMU not found"; exit 1; }
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
rc_all=0

arm() {  # $1 = name, $2 = kernel file ('' for plain), $3 = old, $4 = new, $5 = expect GREEN|RED
    local d="$WORK/$1" log="$WORK/$1.uart" p f v
    mkdir -p "$d"
    tar -C "$ROOT" --exclude=./.git -cf - . | tar -C "$d" -xf -   # whole tree (~17 MB)
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
    ( cd "$d/boot/aarch64" && make clean ) > "$WORK/$1.build" 2>&1
    if ! ( cd "$d/boot/aarch64" && make EXTRA_CFLAGS=-DPK_TKCONF ) >> "$WORK/$1.build" 2>&1; then
        tail -20 "$WORK/$1.build"; echo "[result] $1 build FAILED"; rc_all=1; return
    fi
    ( cd "$d/boot/aarch64" && timeout -k 3 "$BOOT_TIMEOUT" "$QEMU" -M virt,gic-version=2 \
        -cpu cortex-a53 -m 256M -serial stdio -display none -no-reboot -nic none \
        -kernel kernel.elf ) > "$log" 2>&1 < /dev/null
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
arm NC-BITCLR eventflag.c $'\t\tif ( (wfmode & TWF_BITCLR) != 0 ) {\n\t\t\tflgcb->flgptn &= ~waiptn;' $'\t\tif ( (wfmode & TWF_BITCLR) != 0 ) {\n\t\t\tflgcb->flgptn = 0;' RED
arm NC-NOINHERIT mutex.c 'knl_change_task_priority(mtxtsk, knl_ctxtsk->priority);' '(void)0;' RED

if [ "$rc_all" -eq 0 ]; then echo "[tk-conform-a64] PASS"; else echo "[tk-conform-a64] FAIL"; fi
exit "$rc_all"
