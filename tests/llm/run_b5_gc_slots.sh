#!/bin/bash
# ---------------------------------------------------------------------------
# run_b5_gc_slots.sh — B5 gc-restart step 2 (research/b5-gc-restart.md §4c):
# the cleanup's slots are freed, a fetch cut short is dropped, a lost kept
# generation does not stop the cleanup. Seconds, no network.
#
#   [gcs-full] [gcs-evict] [gcs-17th] [gcs-fresh] [gcs-pend] [gcs-pend2] [gcs-lost] [gcs-holds]
#   [gcs-vic] [gcs-vic2] (step 3: a kill between a note's write and its drop; a victim already dropped)
#   (b5_gc_slots_test.c; gossip_learn.c built with -DGL_GC_TEST, test hooks only)
#
# Falsifiers, each must turn exactly its checks red:
#   -DB5_NC_GC_NO_EVICT   [gcs-evict] [gcs-17th]
#   -DB5_NC_GC_NO_PEND    [gcs-pend] [gcs-pend2]
#   -DB5_NC_GC_KEEP_LOST  [gcs-lost] [gcs-holds] [gcs-vic] [gcs-vic2] (no guard table after the lost index)
#   -DB5_NC_GC_NO_VIC     [gcs-vic]
#
#   bash tests/llm/run_b5_gc_slots.sh      exit 0 = PASS
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

kc() { $CC $KFLAGS -c "$1" -o "$WORK/$2" || { echo "[build] FAILED on $1"; exit 1; }; }
kc "$AC/pfs_block.c"      pfs_block.o
kc "$AC/pfs_repl.c"       pfs_repl.o
kc "$AC/pfs_dag.c"        pfs_dag.o
kc "$AC/arkfs.c"          arkfs.o
kc "$AL/pfs_ark.c"        pfs_ark.o
kc "$AL/pfs_durable.c"    pfs_durable.o
kc "$ROOT/relay/sha256.c" sha256.o
$CC $PFLAGS -c "$LLM/student.c" -o "$WORK/student.o" || { echo "[build] FAILED on student.c"; exit 1; }
$CC $PFLAGS -c "$HERE/b5_gc_slots_test.c"       -o "$WORK/test.o" || { echo "[build] FAILED on test"; exit 1; }
$CC $PFLAGS -c "$HERE/student_blob_solo_shim.c" -o "$WORK/shim.o" || { echo "[build] FAILED on shim"; exit 1; }
COMMON="$WORK/shim.o $WORK/student.o $WORK/pfs_block.o $WORK/pfs_repl.o $WORK/pfs_dag.o \
        $WORK/arkfs.o $WORK/pfs_ark.o $WORK/pfs_durable.o $WORK/sha256.o"

arm() { # arm <name> <extra cflags>: build and run, log in $WORK/<name>.log
    $CC $KFLAGS -DGL_GC_TEST $2 -c "$AC/gossip_learn.c" -o "$WORK/gl_$1.o" || { echo "[build] FAILED ($1)"; exit 1; }
    $CC -no-pie -Wl,--gc-sections "$WORK/test.o" "$WORK/gl_$1.o" $COMMON -o "$WORK/t_$1" \
        || { echo "[link] FAILED ($1)"; exit 1; }
    rm -rf "$WORK/pfs"; mkdir -p "$WORK/pfs"
    PKERNEL_PFS_DIR="$WORK/pfs" "$WORK/t_$1" > "$WORK/$1.log" 2>&1
}

echo "[run] B5 gc slots on the code as it is ..."
arm plain ""; RC=$?
grep -E '^\[gcs\]|^  (PASS|FAIL) |^\[setup\]' "$WORK/plain.log"

NC_OK=1
nc() { # nc <name> <flag> <the checks that must be red, space separated>
    arm "$1" "$2"
    local want="$3" got
    got=$(grep -o '^  FAIL  \[gcs-[a-z0-9]*\]' "$WORK/$1.log" | sed 's/.*\[\(.*\)\]/\1/' | sort | tr '\n' ' ' | sed 's/ $//')
    echo "[falsifier] $2: red = {$got}, want {$want}"
    [ "$got" = "$want" ] || { echo "[falsifier] FAIL: $2 did not turn exactly {$want} red"; NC_OK=0; }
}
echo ""
nc noevict -DB5_NC_GC_NO_EVICT  "gcs-17th gcs-evict"
nc nopend  -DB5_NC_GC_NO_PEND   "gcs-pend gcs-pend2"
# KEEP_LOST: the guard never gets built again after the lost index, so
# gl_gc_holds (2b) has no table either = [gcs-holds] goes red with it
# (and no guard = the victim [gcs-vic] leaves after the restart is not dropped either)
nc keeplost -DB5_NC_GC_KEEP_LOST "gcs-holds gcs-lost gcs-vic gcs-vic2"
nc novic    -DB5_NC_GC_NO_VIC    "gcs-vic"

echo ""
if [ "$RC" -eq 0 ] && [ "$NC_OK" = 1 ]; then echo "[result] PASS"; exit 0; fi
echo "[result] FAIL (test=$RC falsifiers_ok=$NC_OK)"; exit 1
