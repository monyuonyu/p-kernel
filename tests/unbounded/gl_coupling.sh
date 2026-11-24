#!/bin/bash
# tests/unbounded/gl_coupling.sh — [unbounded-gl]: N3 step 3, the first table =
# gossip_learn (research/n3-gl-region.md, gates fixed 2026-11-24 before any code).
#
#   G1  gossip_learn.o (the REAL arch/common/gossip_learn.c, hosted x86_64 flags)
#       has the same static data (size: data + bss) at DNODE_MAX 64 / 256 / 1024
#       = the peer cache and the in-process scratch are not sized by the fleet
#
#   N3G_NC=1  negative control: force one [DNODE_MAX] array into gossip_learn.o => G1 RED
# Exit 0 = G1 PASS. Written test first: on master ba1e269e G1 is RED (step 3 not
# built; gl_model, gl_pcache, sh_x, sh_y, sh_n, gl_phave grow). It prints what
# grew, so a red names it.
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
case "${N3G_NC:-0}" in
1)  printf '#include "drpc.h"\nstatic unsigned char n3g_nc_fleet[DNODE_MAX] __attribute__((used));\n' > "$B/nc.h"
    NC="-include $B/nc.h"
    echo "[unbounded-gl] negative control 1: one [DNODE_MAX] array forced into gossip_learn.o" ;;
esac
declare -A DB
for n in 64 256 1024; do
    if ! $CC $CF $INC $NC -DDNODE_MAX=$n -c "$ROOT/arch/common/gossip_learn.c" -o "$B/g$n.o" 2> "$B/w$n.txt"; then
        echo "[unbounded-gl] FAIL build at DNODE_MAX=$n"; sed 's/^/    /' "$B/w$n.txt" | head -8
        exit 1
    fi
    DB[$n]=$(size "$B/g$n.o" | awk 'NR==2 {print $2 + $3}')
    nm -S "$B/g$n.o" > "$B/y$n.txt"
done
echo "[unbounded-gl] gossip_learn.o data+bss at DNODE_MAX 64/256/1024: ${DB[64]} / ${DB[256]} / ${DB[1024]}"
if [ "${DB[64]}" = "${DB[256]}" ] && [ "${DB[256]}" = "${DB[1024]}" ]; then
    echo "[unbounded-gl] G1 PASS: gossip_learn's static data does not grow with the fleet"
    exit 0
fi
echo "[unbounded-gl] G1 FAIL: gossip_learn's static data grows with DNODE_MAX; the tables that grew (64 -> 1024 bytes):"
python3 -c '
import sys
def load(p):
    d = {}
    for l in open(p):
        f = l.split()
        if len(f) == 4 and f[2] in "bBdD": d[f[3]] = int(f[1], 16)
    return d
a, b = load(sys.argv[1]), load(sys.argv[2])
for k in sorted(set(a) | set(b)):
    if a.get(k, 0) != b.get(k, 0): print("    %-28s %8d -> %8d" % (k, a.get(k, 0), b.get(k, 0)))
' "$B/y64.txt" "$B/y1024.txt"
exit 1
