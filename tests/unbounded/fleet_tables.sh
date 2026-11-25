#!/bin/bash
# tests/unbounded/fleet_tables.sh — which static tables still grow with the
# fleet (N3, research/n3-fleet-tables.md). A report, not a gate: every
# arch/common/*.c and arch/linux/*.c is compiled with the hosted x86_64 flags
# at DNODE_MAX 64 and 1024, and the objects whose data+bss differ are printed
# with the symbols that moved (largest growth first).
#
# Caveat: at DNODE_MAX >= 256 a `for (UB n = 0; n < DNODE_MAX; n++)` never
# ends, the compiler drops what follows, and an object can SHRINK (dkva). A
# shrink is that artifact, not a table made smaller.
#
#   bash tests/unbounded/fleet_tables.sh     always exit 0 (exit 1 = no object built)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC=${CC:-gcc}
B="$(mktemp -d)"
trap 'rm -rf "$B"' EXIT
M=$ROOT/kernel/mtkernel3
INC="-I$ROOT/boot/linux_x86_64 -I$ROOT/arch/linux/x86_64/include -I$ROOT/arch/linux/include \
 -I$ROOT/arch/common/include/lp64 -I$ROOT/arch/common/include -I$ROOT/relay -I$ROOT/arch/common/llm \
 -I$M/kernel/knlinc -I$M/kernel/tkernel -I$M/include -I$M/include/tm -I$M/include/compat -I$M/config"
CF="-w -O1 -no-pie -fno-pie -D_APP_X86_64_ -D_APP_LINUX_ -D_TK_HOSTED_LIBC_ -D_LINUX_X86_64_"
tot64=0; tot1k=0; nbuilt=0; nobuild=""
for f in "$ROOT"/arch/common/*.c "$ROOT"/arch/linux/*.c; do
    b=$(basename "$f" .c)
    if ! $CC $CF $INC -DDNODE_MAX=64 -c "$f" -o "$B/$b.64.o" 2>/dev/null \
       || ! $CC $CF $INC -DDNODE_MAX=1024 -c "$f" -o "$B/$b.1024.o" 2>/dev/null; then
        nobuild="$nobuild $b"; continue
    fi
    nbuilt=$((nbuilt + 1))
    a=$(size "$B/$b.64.o" | awk 'NR==2 {print $2 + $3}')
    c=$(size "$B/$b.1024.o" | awk 'NR==2 {print $2 + $3}')
    tot64=$((tot64 + a)); tot1k=$((tot1k + c))
    if [ "$a" != "$c" ]; then
        echo "$((c - a)) $b $a $c" >> "$B/grow.txt"
        nm -S "$B/$b.64.o" > "$B/$b.64.nm"; nm -S "$B/$b.1024.o" > "$B/$b.1024.nm"
    fi
    rm -f "$B/$b.64.o" "$B/$b.1024.o"
done
[ "$nbuilt" -gt 0 ] || { echo "[fleet-tables] no object built"; exit 1; }
echo "[fleet-tables] data+bss of $nbuilt objects at DNODE_MAX 64 -> 1024: $tot64 -> $tot1k (+$((tot1k - tot64)))"
[ -n "$nobuild" ] && echo "[fleet-tables] not built with these flags:$nobuild"
[ -f "$B/grow.txt" ] || { echo "[fleet-tables] nothing grows"; exit 0; }
sort -n -r "$B/grow.txt" | while read -r d b a c; do
    echo "[fleet-tables] $b $a -> $c ($d)"
    python3 -c '
import sys
def load(p):
    d = {}
    for l in open(p):
        f = l.split()
        if len(f) == 4 and f[2] in "bBdD": d[f[3]] = int(f[1], 16)
    return d
a, b = load(sys.argv[1]), load(sys.argv[2])
moved = [(b.get(k, 0) - a.get(k, 0), k) for k in set(a) | set(b) if a.get(k, 0) != b.get(k, 0)]
for d, k in sorted(moved, key=lambda t: (-t[0], t[1]))[:8]:
    print("    %-32s %9d -> %9d" % (k, a.get(k, 0), b.get(k, 0)))
' "$B/$b.64.nm" "$B/$b.1024.nm"
done
exit 0
