#!/bin/bash
# tests/unbounded/row_coupling.sh — [unbounded-rows]: N3 step 4, pk_node_row.h.
#
# Design: docs/architecture/30-module/research/n3-row-ring.md (gates R1–R3,
# fixed before the rows were changed; on master 69403bdf R1 and R2 are RED).
#
#   R1  the five users of pk_node_row (drpc, r3_incontext, pfs_repl, region,
#       protect; the REAL arch/common/*.c, hosted x86_64 flags) have the same
#       static data at DNODE_MAX 64 / 256 / 1024: data+bss equal AND no data/bss
#       symbol changes size (a symbol that vanishes, e.g. code dropped after a
#       loop that never ends, also counts as a change)
#   R2  in what the hosted build compiles of those five files, no 8-bit
#       (UB/U1) value is compared with DNODE_MAX / DNODE_ROWS / MW_ROWS: at 256
#       such a loop never ends. Seen by the compiler (-Wtype-limits at
#       DNODE_MAX=1024), so the bare-metal branches (ids 1..8, crown) are not counted
#
#   N3R_NC=1  negative control: force one [DNODE_MAX] array into region.o => R1 RED
#   N3R_NC=2  negative control: scan one extra .c with a UB loop to DNODE_MAX => R2 RED
# Exit 0 = R1 and R2 PASS. A red prints what grew and which loops.
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
USERS="drpc r3_incontext pfs_repl region protect"
NCF=""
EXTRA=""
case "${N3R_NC:-0}" in
1)  printf '#include "drpc.h"\nstatic unsigned char n3r_nc_fleet[DNODE_MAX] __attribute__((used));\n' > "$B/nc.h"
    NCF="$B/nc.h"
    echo "[unbounded-rows] negative control 1: one [DNODE_MAX] array forced into region.o" ;;
2)  printf 'typedef unsigned char UB;\nint n3r_nc(int *t) { int k = 0; for (UB n = 0; n < DNODE_MAX; n++) k += t[n]; return k; }\n' > "$B/n3r_nc.c"
    EXTRA="$B/n3r_nc.c"
    echo "[unbounded-rows] negative control 2: one extra .c with a UB loop to DNODE_MAX" ;;
esac
fails=0
r1=0
for b in $USERS; do
    line=""
    for n in 64 256 1024; do
        inc=""
        [ -n "$NCF" ] && [ "$b" = region ] && inc="-include $NCF"
        if ! $CC $CF $INC $inc -DDNODE_MAX=$n -c "$ROOT/arch/common/$b.c" -o "$B/$b.$n.o" 2> "$B/$b.$n.w"; then
            echo "[unbounded-rows] FAIL build $b at DNODE_MAX=$n"; sed 's/^/    /' "$B/$b.$n.w" | head -6
            exit 1
        fi
        line="$line $(size "$B/$b.$n.o" | awk 'NR==2 {print $2 + $3}')"
        nm -S "$B/$b.$n.o" > "$B/$b.$n.nm"
    done
    if python3 -c '
import sys
def load(p):
    d = {}
    for l in open(p):
        f = l.split()
        if len(f) == 4 and f[2] in "bBdD": d[f[3]] = int(f[1], 16)
    return d
base = load(sys.argv[1]); bad = 0
for p in sys.argv[2:]:
    o = load(p)
    for k in sorted(set(base) | set(o), key=lambda k: -(o.get(k, 0) - base.get(k, 0))):
        if base.get(k, 0) != o.get(k, 0):
            if bad < 12: print("    %-32s %8d -> %8d  (%s)" % (k, base.get(k, 0), o.get(k, 0), p.rsplit(".", 2)[-2]))
            bad += 1
sys.exit(1 if bad else 0)
' "$B/$b.64.nm" "$B/$b.256.nm" "$B/$b.1024.nm" > "$B/$b.moved"; then
        echo "[unbounded-rows] $b data+bss 64/256/1024:$line  same"
    else
        echo "[unbounded-rows] $b data+bss 64/256/1024:$line  MOVED:"
        cat "$B/$b.moved"
        r1=$((r1 + 1))
    fi
done
if [ "$r1" = 0 ]; then
    echo "[unbounded-rows] R1 PASS: the row users' static data does not grow with the fleet"
else
    echo "[unbounded-rows] R1 FAIL: $r1 of 5 row users grow with DNODE_MAX"
    fails=$((fails + 1))
fi
# R2: what the hosted build compiles (not the bare-metal #ifdef branches): at
# DNODE_MAX=1024 an 8-bit counter compared to the fleet is "always true due to
# limited range" (-Wtype-limits). Count those warnings in the five files.
: > "$B/tl.txt"
for f in $(for b in $USERS; do echo "$ROOT/arch/common/$b.c"; done) $EXTRA; do
    $CC -O1 -no-pie -fno-pie -D_APP_X86_64_ -D_APP_LINUX_ -D_TK_HOSTED_LIBC_ -D_LINUX_X86_64_ \
        $INC -DDNODE_MAX=1024 -Wtype-limits -c "$f" -o "$B/tl.o" 2>&1 \
        | grep 'always true due to limited range' >> "$B/tl.txt"
done
python3 -c '
import sys
hits = []
for l in open(sys.argv[1]):
    loc = l.split(": warning")[0].rsplit("/", 1)[-1]
    if loc not in hits: hits.append(loc)
print("[unbounded-rows] 8-bit comparisons with the fleet (always true at 1024) in the row users: %d" % len(hits))
for h in hits[:30]: print("    " + h)
sys.exit(1 if hits else 0)
' "$B/tl.txt"
if [ "$?" = 0 ]; then
    echo "[unbounded-rows] R2 PASS: no 8-bit loop counts to the fleet"
else
    echo "[unbounded-rows] R2 FAIL: an 8-bit value is compared with the fleet (a loop never ends at 256, a check is always true)"
    fails=$((fails + 1))
fi
if [ "$fails" -eq 0 ]; then echo "[unbounded-rows] ALL PASS"; else echo "[unbounded-rows] RED ($fails)"; fi
exit "$fails"
