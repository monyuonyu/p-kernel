#!/bin/bash
# tests/unbounded/swim_coupling.sh — [unbounded-swim]: N3 step 1, the SWIM tables.
#
# Design: docs/architecture/30-module/research/n3-swim-region.md (gates S1–S3,
# fixed before the implementation was measured).
#
# Compiles the REAL arch/common/swim.c with the hosted x86_64 flags at
# DNODE_MAX 64 / 256 / 1024 (DREGION_MAX left at its default) and checks:
#   S1  the static data of swim.o (size: data + bss) is the same at all three
#       = no per-node table in swim.c is sized by the fleet any more
#   S2  swim.c compiles at DNODE_MAX 256 and 1024 with no -Wtype-limits warning
#       = no 8-bit counter loops to DNODE_MAX (they never end at 256 or more)
# It prints the symbols that grew, so a red names the table.
#
#   N3S_NC=1  negative control: force one [DNODE_MAX] array into swim.o
#             (-include a header). S1 must go RED.
# Exit 0 = S1 and S2 PASS. On master e4ca62c8 (before N3 step 1) both are RED:
# data+bss 11028 / 13716 / 24468, four always-true loops.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC=${CC:-gcc}
B="$(mktemp -d)"
trap 'rm -rf "$B"' EXIT
M=$ROOT/kernel/mtkernel3
INC="-I$ROOT/boot/linux_x86_64 -I$ROOT/arch/linux/x86_64/include -I$ROOT/arch/linux/include \
 -I$ROOT/arch/common/include/lp64 -I$ROOT/arch/common/include -I$ROOT/relay \
 -I$M/kernel/knlinc -I$M/kernel/tkernel -I$M/include -I$M/include/tm -I$M/include/compat -I$M/config"
CF="-Wall -Wextra -g -O1 -no-pie -fno-pie -D_APP_X86_64_ -D_APP_LINUX_ -D_TK_HOSTED_LIBC_ -D_LINUX_X86_64_"
NC=""
if [ "${N3S_NC:-0}" = 1 ]; then
    printf '#include "drpc.h"\nstatic unsigned char n3s_nc_fleet[DNODE_MAX] __attribute__((used));\n' > "$B/nc.h"
    NC="-include $B/nc.h"
    echo "[unbounded-swim] negative control: one [DNODE_MAX] array forced into swim.o"
fi
fails=0
declare -A DB
for n in 64 256 1024; do
    if ! $CC $CF $INC $NC -DDNODE_MAX=$n -c "$ROOT/arch/common/swim.c" -o "$B/s$n.o" 2> "$B/w$n.txt"; then
        echo "[unbounded-swim] FAIL build at DNODE_MAX=$n"; sed 's/^/    /' "$B/w$n.txt" | head -8
        exit 1
    fi
    DB[$n]=$(size "$B/s$n.o" | awk 'NR==2 {print $2 + $3}')
    nm -S "$B/s$n.o" | awk '$3 ~ /^[bBdD]$/ {print $4, strtonum("0x" $2)}' | sort > "$B/y$n.txt"
done
echo "[unbounded-swim] swim.o data+bss at DNODE_MAX 64/256/1024: ${DB[64]} / ${DB[256]} / ${DB[1024]}"
if [ "${DB[64]}" = "${DB[256]}" ] && [ "${DB[256]}" = "${DB[1024]}" ]; then
    echo "[unbounded-swim] S1 PASS: swim's static data does not grow with the fleet"
else
    echo "[unbounded-swim] S1 FAIL: swim's static data grows with DNODE_MAX; the tables that grew (64 -> 1024 bytes):"
    join -a1 -a2 -e 0 -o 0,1.2,2.2 "$B/y64.txt" "$B/y1024.txt" | awk '$2 != $3 {printf "    %-28s %7d -> %7d\n", $1, $2, $3}'
    fails=$((fails + 1))
fi
wl=$(cat "$B/w256.txt" "$B/w1024.txt" | grep -c 'Wtype-limits')
if [ "$wl" -eq 0 ]; then
    echo "[unbounded-swim] S2 PASS: no always-true loop at DNODE_MAX 256/1024"
else
    echo "[unbounded-swim] S2 FAIL: $wl always-true comparisons (an 8-bit counter to DNODE_MAX never ends at 256+):"
    grep -h 'Wtype-limits' "$B/w256.txt" | sed 's/^/    /'
    fails=$((fails + 1))
fi
if [ "$fails" -eq 0 ]; then echo "[unbounded-swim] ALL PASS"; else echo "[unbounded-swim] RED ($fails)"; fi
exit "$fails"
