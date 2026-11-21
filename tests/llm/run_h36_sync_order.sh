#!/bin/bash
# ---------------------------------------------------------------------------
# run_h36_sync_order.sh — test-holes H36: the publish's syncfs must come
# BEFORE its descriptor (B5 D4, research/b5-m-round.md §4). Seconds, no network.
#
#   [h36-lazy] [h36-desc] [h36-order] [h36-roundtrip]  (h36_sync_order_test.c)
#
# The order is seen through the linker: --wrap on pfs_dur_write_lazy,
# pfs_dur_sync and pfs_dag_save (calls between object files only).
# Falsifier x5 (audit-196, pk-scratch/a196/x5.py): a copy of gossip_learn.c
# with the syncfs moved after the descriptor save; it must turn [h36-order]
# alone red. The product code is not changed by this test.
#
#   bash tests/llm/run_h36_sync_order.sh      exit 0 = PASS
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

AC="$ROOT/arch/common"
AL="$ROOT/arch/linux"
LLM="$AC/llm"

INCS="-I$ROOT/boot/linux \
      -I$AL/aarch64/include \
      -I$AL/include \
      -I$ROOT/arch/aarch64/include \
      -I$AC/include/lp64 \
      -I$AC/include \
      -I$ROOT/relay \
      -I$ROOT/kernel/mtkernel3/kernel/knlinc \
      -I$ROOT/kernel/mtkernel3/kernel/tkernel \
      -I$ROOT/kernel/mtkernel3/include \
      -I$ROOT/kernel/mtkernel3/include/tm \
      -I$ROOT/kernel/mtkernel3/include/compat \
      -I$ROOT/kernel/mtkernel3/config"
KDEFS="-D_APP_AARCH64_ -D_APP_LINUX_ -D_TK_HOSTED_LIBC_ -D_LINUX_AARCH64_ -DPK_NOSW_OFF"
KFLAGS="-Wall -Wextra -g -O1 -no-pie -fno-pie -fno-stack-protector -fno-common \
        -std=gnu11 -ffp-contract=off -ffunction-sections -fdata-sections \
        -Werror=vla $KDEFS $INCS"
PFLAGS="-Wall -Wextra -g -O1 -no-pie -fno-pie -std=gnu11 -ffp-contract=off \
        -ffunction-sections -fdata-sections -Werror=vla -I$LLM"
WRAP="-Wl,--wrap=pfs_dur_write_lazy -Wl,--wrap=pfs_dur_sync -Wl,--wrap=pfs_dag_save"

kc() { $CC $KFLAGS -c "$1" -o "$WORK/$2" || { echo "[build] FAILED on $1"; exit 1; }; }
kc "$AC/gossip_learn.c"   gossip_learn.o
kc "$AC/pfs_block.c"      pfs_block.o
kc "$AC/pfs_repl.c"       pfs_repl.o
kc "$AC/pfs_dag.c"        pfs_dag.o
kc "$AC/arkfs.c"          arkfs.o
kc "$AL/pfs_ark.c"        pfs_ark.o
kc "$AL/pfs_durable.c"    pfs_durable.o
kc "$ROOT/relay/sha256.c" sha256.o
$CC $PFLAGS -c "$LLM/student.c" -o "$WORK/student.o" || { echo "[build] FAILED on student.c"; exit 1; }
$CC $PFLAGS -c "$HERE/h36_sync_order_test.c"    -o "$WORK/test.o" || { echo "[build] FAILED on test"; exit 1; }
$CC $PFLAGS -c "$HERE/student_blob_solo_shim.c" -o "$WORK/shim.o" || { echo "[build] FAILED on shim"; exit 1; }

COMMON="$WORK/shim.o $WORK/student.o $WORK/pfs_block.o $WORK/pfs_repl.o $WORK/pfs_dag.o \
        $WORK/arkfs.o $WORK/pfs_ark.o $WORK/pfs_durable.o $WORK/sha256.o"
lk() { # lk <gossip_learn.o> <out>
    $CC -no-pie -Wl,--gc-sections $WRAP "$WORK/test.o" "$1" $COMMON -o "$2" \
        || { echo "[link] FAILED ($2)"; exit 1; }
}
lk "$WORK/gossip_learn.o" "$WORK/h36"

run() { # run <exe> <log>
    rm -rf "$WORK/pfs"; mkdir -p "$WORK/pfs"
    PKERNEL_PFS_DIR="$WORK/pfs" "$1" > "$2" 2>&1
}

echo "[run] H36 on the code as it is ..."
run "$WORK/h36" "$WORK/h36.log"; RC=$?
grep -E '^\[h36\]|^  (PASS|FAIL) ' "$WORK/h36.log"

# falsifier x5: the syncfs after the descriptor (same anchors as audit-196 x5.py)
cp "$AC/gossip_learn.c" "$WORK/gl_x5.c"
python3 - "$WORK/gl_x5.c" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
a = '    if (gl_st_sync() != 0) return -1;\n\n    /* descriptor under ONE named ref'
b = '    /* x5: sync moved after the descriptor */\n\n    /* descriptor under ONE named ref'
# a2 is the descriptor save itself, not what follows it: B5 ① (feat/b5-gc,
# master 3f45c6c5) put gl_gc_note between the save and the return, the old
# anchor (the save then "return (INT)nchunk;") stopped matching and CI's
# falsifier step went red ("anchor not found", run 44863534347)
a2 = 'if (pfs_dag_save((const UB *)nm, nl, &gl_st_desc, (UW)sizeof gl_st_desc) != PFS_OK)\n        return -1;\n'
b2 = a2 + '    (void)gl_st_sync(); /* x5 */\n'
if s.count(a) != 1 or s.count(a2) != 1:
    print('[falsifier] anchor not found'); sys.exit(1)
open(p, 'w').write(s.replace(a, b).replace(a2, b2))
PY
[ -s "$WORK/gl_x5.c" ] || exit 1
grep -q 'x5: sync moved' "$WORK/gl_x5.c" || { echo "[falsifier] FAIL: x5 patch did not apply"; exit 1; }
$CC $KFLAGS -I"$AC" -c "$WORK/gl_x5.c" -o "$WORK/gl_x5.o" || { echo "[build] FAILED on x5"; exit 1; }
lk "$WORK/gl_x5.o" "$WORK/h36_x5"
echo ""
echo "[run] falsifier x5 (syncfs after the descriptor) ..."
run "$WORK/h36_x5" "$WORK/x5.log"
grep -E '^\[h36\]|^  (PASS|FAIL) ' "$WORK/x5.log"
NC_OK=0
if grep -q '^  FAIL  \[h36-order\]' "$WORK/x5.log" && [ "$(grep -c '^  FAIL  ' "$WORK/x5.log")" = 1 ]; then
    echo "[falsifier] x5 turns only [h36-order] red (as it must)"; NC_OK=1
else
    echo "[falsifier] FAIL: x5 did not turn exactly [h36-order] red"
fi

echo ""
if [ "$RC" -eq 0 ] && [ "$NC_OK" = 1 ]; then echo "[result] PASS"; exit 0; fi
echo "[result] FAIL (test=$RC falsifier_ok=$NC_OK)"; exit 1
