#!/bin/bash
# tests/unbounded/dnode_coupling.sh — [unbounded-dnode]: N3 step 2, dnode_table.
#
# Design: docs/architecture/30-module/research/n3-dnode-region.md (gates D1–D3,
# fixed in af0ff1fb before this script ran anywhere).
#
#   D1  drpc.o (the REAL arch/common/drpc.c, hosted x86_64 flags) has the same
#       static data (size: data + bss) at DNODE_MAX 64 / 256 / 1024
#       = the node table is not sized by the fleet any more
#   D2  outside drpc.c and swim.c, no code line under arch/ indexes
#       dnode_table[ directly (comment lines and a comment after code are
#       skipped; a string literal with "//" in it may hide a use) = everyone goes through
#       the accessors of drpc.h, so the rows can move without touching them
#
#   D2c (2a, audit-211 4b) outside the owners the COMPILER refuses
#       dnode_table (drpc.h poisons it after its accessor macros): five shapes
#       (direct, a line starting with "*", an alias pointer, "dnode_table [",
#       sizeof) must fail with "poisoned"; the same probe through
#       dnode_state/ip/missed/node_id and DNODE_TABLE_BYTES must compile.
#       D2 stays as the helper that names the lines.
#
#   N3D_NC=1  negative control: force one [DNODE_MAX] array into drpc.o => D1 RED
#   N3D_NC=2  negative control: scan one extra .c that indexes dnode_table[ => D2 RED
# Exit 0 = D1 and D2 PASS. Written test first: on master 64d51ff2 both are
# expected RED (step 2 not built). It prints what grew and where the direct
# uses are, so a red names them.
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
EXTRA=""
case "${N3D_NC:-0}" in
1)  printf '#define DRPC_TABLE_OWNER 1\n#include "drpc.h"\nstatic unsigned char n3d_nc_fleet[DNODE_MAX] __attribute__((used));\n' > "$B/nc.h"
    NC="-include $B/nc.h"
    echo "[unbounded-dnode] negative control 1: one [DNODE_MAX] array forced into drpc.o" ;;
2)  printf '#include "drpc.h"\nint n3d_nc_direct(void) { return dnode_table[1].state; }\n' > "$B/n3d_nc.c"
    EXTRA="$B/n3d_nc.c"
    echo "[unbounded-dnode] negative control 2: one extra .c that indexes dnode_table[ directly" ;;
esac
fails=0
declare -A DB
for n in 64 256 1024; do
    if ! $CC $CF $INC $NC -DDNODE_MAX=$n -c "$ROOT/arch/common/drpc.c" -o "$B/d$n.o" 2> "$B/w$n.txt"; then
        echo "[unbounded-dnode] FAIL build at DNODE_MAX=$n"; sed 's/^/    /' "$B/w$n.txt" | head -8
        exit 1
    fi
    DB[$n]=$(size "$B/d$n.o" | awk 'NR==2 {print $2 + $3}')
    nm -S "$B/d$n.o" > "$B/y$n.txt"
done
echo "[unbounded-dnode] drpc.o data+bss at DNODE_MAX 64/256/1024: ${DB[64]} / ${DB[256]} / ${DB[1024]}"
if [ "${DB[64]}" = "${DB[256]}" ] && [ "${DB[256]}" = "${DB[1024]}" ]; then
    echo "[unbounded-dnode] D1 PASS: drpc's static data does not grow with the fleet"
else
    echo "[unbounded-dnode] D1 FAIL: drpc's static data grows with DNODE_MAX; the tables that grew (64 -> 1024 bytes):"
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
    if a.get(k, 0) != b.get(k, 0): print("    %-28s %7d -> %7d" % (k, a.get(k, 0), b.get(k, 0)))
' "$B/y64.txt" "$B/y1024.txt"
    fails=$((fails + 1))
fi
# D2: direct uses outside the two owners. A comment line (first non-blank is
# "*", "/*" or "//") is not counted; a use after code on the same line is.
python3 -c '
import os, re, sys
root, extra = sys.argv[1], sys.argv[2:]
own = {"arch/common/drpc.c", "arch/common/swim.c"}
files = []
for d, _, fs in os.walk(os.path.join(root, "arch")):
    for f in fs:
        if f.endswith((".c", ".h")):
            p = os.path.join(d, f)
            if os.path.relpath(p, root) not in own: files.append(p)
files += extra
hits = {}
for p in sorted(files):
    for i, l in enumerate(open(p, errors="replace"), 1):
        s = l.strip()
        if s.startswith(("*", "/*", "//")): continue
        s = re.sub(r"/\*.*?(\*/|$)", "", s).split("//")[0]   # the comment after code
        if "dnode_table[" in s:
            hits.setdefault(os.path.relpath(p, root) if p.startswith(root) else os.path.basename(p), []).append(i)
n = sum(len(v) for v in hits.values())
print("[unbounded-dnode] direct dnode_table[ lines outside drpc.c/swim.c: %d in %d files" % (n, len(hits)))
for k in sorted(hits, key=lambda k: -len(hits[k]))[:12]:
    print("    %-40s %3d  (first line %d)" % (k, len(hits[k]), hits[k][0]))
sys.exit(1 if n else 0)
' "$ROOT" $EXTRA
if [ "$?" = 0 ]; then
    echo "[unbounded-dnode] D2 PASS: only drpc.c and swim.c index dnode_table directly"
else
    echo "[unbounded-dnode] D2 FAIL: code outside drpc.c/swim.c indexes dnode_table directly (use the drpc.h accessors)"
    fails=$((fails + 1))
fi
# D2c (2a, audit-211 4b): the compiler, not the grep, keeps the table behind
# the door. Outside the owners, a .c that includes drpc.h and names
# dnode_table in any shape (the auditor's b1-b3 and sizeof) must NOT compile,
# and the stderr must say "poisoned"; the same .c through the door must compile.
d2c=0
printf '#include "drpc.h"\nint p(void) { return dnode_state(1) + (int)dnode_ip(1) + dnode_missed(1) + (int)dnode_node_id(1) + (int)DNODE_TABLE_BYTES; }\n' > "$B/p_ok.c"
printf '#include "drpc.h"\nint p(void) { return dnode_table[1].state; }\n' > "$B/p_b0.c"
printf '#include "drpc.h"\nint p(void) { int x;\n *&x = dnode_table[1].state; return x; }\n' > "$B/p_b1.c"
printf '#include "drpc.h"\nint p(void) { DNODE *t = dnode_table; return t[1].state; }\n' > "$B/p_b2.c"
printf '#include "drpc.h"\nint p(void) { return dnode_table [1].state; }\n' > "$B/p_b3.c"
printf '#include "drpc.h"\nint p(void) { return (int)sizeof(dnode_table); }\n' > "$B/p_b4.c"
if $CC $CF $INC -c "$B/p_ok.c" -o "$B/p_ok.o" 2> "$B/p_ok.txt"; then
    echo "[unbounded-dnode] D2c door: a .c through dnode_state/ip/missed/node_id/DNODE_TABLE_BYTES compiles"
else
    echo "[unbounded-dnode] D2c FAIL: the door does not compile"; sed 's/^/    /' "$B/p_ok.txt" | head -4
    d2c=1
fi
for b in b0 b1 b2 b3 b4; do
    if $CC $CF $INC -c "$B/p_$b.c" -o "$B/p_$b.o" 2> "$B/p_$b.txt"; then
        echo "[unbounded-dnode] D2c FAIL: $b names dnode_table outside the owners and still compiles"
        d2c=1
    elif grep -q poisoned "$B/p_$b.txt"; then
        echo "[unbounded-dnode] D2c $b: refused by the compiler (poisoned)"
    else
        echo "[unbounded-dnode] D2c FAIL: $b did not compile, but not because of the poison:"; sed 's/^/    /' "$B/p_$b.txt" | head -3
        d2c=1
    fi
done
if [ "$d2c" = 0 ]; then
    echo "[unbounded-dnode] D2c PASS: outside drpc.c/swim.c the compiler refuses dnode_table; the door works"
else
    fails=$((fails + 1))
fi
if [ "$fails" -eq 0 ]; then echo "[unbounded-dnode] ALL PASS"; else echo "[unbounded-dnode] RED ($fails)"; fi
exit "$fails"
