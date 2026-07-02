#!/bin/bash
# ---------------------------------------------------------------------------
# run_ss3_blob.sh — host cert for the SS-3 student-blob TRANSPORT (STEP 1 gate)
#                   docs/architecture/student-blob-transport.md §3.
#
# Proves the content-addressed manifest transport (gl_student_publish /
# gl_student_fetch, the _TK_HOSTED_LIBC_ bodies in arch/common/gossip_learn.c)
# round-trips a REAL S-tier student blob BIT-EXACTLY in ONE process with ZERO
# network (SOLO, drpc_my_node==0xFF), then merges it via the UNCHANGED crown
# math (st_merge_cohort). Greppable [tag] PASS/FAIL.
#
#   [ss3-blob-roundtrip]          publish->fetch->memcmp==0 (482 chunks, depth 2)
#   [ss3-blob-merge]              recovered blob merges == direct blob (fidelity)
#   [ss3-blob-roundtrip-falsify]  dropped chunk -> fetch <0, out unconsumed
#   [ss3-blob-refuse-toobig]      over-cap blob refused (-1), never truncated
#
# [finding] The P0 block store is PFS_MAX_BLOCKS=64; the S blob is 482 chunks.
# A memory-only store CANNOT hold a whole student blob (pfs_put PFS_E_FULL at
# the 65th block) — so this cert (and the eventual [live] fetcher) needs the
# eviction-capable durable ARK backend. RAM is a 64-slot cache; every block is
# also in the ARK log; a pfs_get cache-miss falls through to ARK. The transport
# bytes are content-addressed and unchanged, so recovery stays bit-exact. This
# is NOT in the design doc's §3 "memory-only SOLO" claim — see the report.
#
# The transport TUs (pfs_block/pfs_repl/pfs_dag/gossip_learn) + arkfs need the
# KERNEL hosted include chain (kernel.h), so they are compiled with boot/linux's
# CFLAGS+INCDIRS. The test + SOLO shim stay in the plain libc world.
#
#   ./run_ss3_blob.sh
# Exit 0 = all certs PASS.
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

# kernel-hosted include chain (mirrors boot/linux/Makefile INCDIRS) ----------
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
KDEFS="-D_APP_AARCH64_ -D_APP_LINUX_ -D_TK_HOSTED_LIBC_ -D_LINUX_AARCH64_"
KFLAGS="-Wall -Wextra -g -O1 -no-pie -fno-pie -fno-stack-protector -fno-common \
        -std=gnu11 -ffp-contract=off -ffunction-sections -fdata-sections \
        -Werror=vla $KDEFS $INCS"
# plain libc world for the test + shim
PFLAGS="-Wall -Wextra -g -O1 -no-pie -fno-pie -std=gnu11 -ffp-contract=off \
        -ffunction-sections -fdata-sections -Werror=vla -I$LLM"

echo "[build] SS-3 blob-transport cert (kernel-hosted transport + ARK; -O1 -ffp-contract=off -Werror=vla)"

# kernel-hosted TUs ----------------------------------------------------------
kc() { # kc <src> <obj>
    $CC $KFLAGS -c "$1" -o "$WORK/$2" || { echo "[build] FAILED on $1"; exit 1; }
}
kc "$AC/gossip_learn.c"        gossip_learn.o
kc "$AC/pfs_block.c"           pfs_block.o
kc "$AC/pfs_repl.c"            pfs_repl.o
kc "$AC/pfs_dag.c"             pfs_dag.o
kc "$AC/arkfs.c"               arkfs.o
kc "$AL/pfs_ark.c"             pfs_ark.o
kc "$AL/pfs_durable.c"         pfs_durable.o
kc "$ROOT/relay/sha256.c"      sha256.o
# student.c (LLM TU) — PLAIN libc flags (it uses the SYSTEM stdint/stdlib, like
# its own run_ss3.sh; the kernel include chain would shadow <stdint.h>). -------
$CC $PFLAGS -c "$LLM/student.c" -o "$WORK/student.o" \
    || { echo "[build] FAILED on student.c"; exit 1; }

# plain libc TUs -------------------------------------------------------------
$CC $PFLAGS -c "$HERE/student_blob_test.c"      -o "$WORK/test.o"  || { echo "[build] FAILED on test"; exit 1; }
$CC $PFLAGS -c "$HERE/student_blob_solo_shim.c" -o "$WORK/shim.o"  || { echo "[build] FAILED on shim"; exit 1; }

# link (gc-sections drops gossip_learn's dtr/reflex-bound functions) ---------
$CC -no-pie -Wl,--gc-sections \
    "$WORK"/test.o "$WORK"/shim.o "$WORK"/student.o \
    "$WORK"/gossip_learn.o "$WORK"/pfs_block.o "$WORK"/pfs_repl.o "$WORK"/pfs_dag.o \
    "$WORK"/arkfs.o "$WORK"/pfs_ark.o "$WORK"/pfs_durable.o "$WORK"/sha256.o \
    -o "$WORK/blob" \
    || { echo "[link] FAILED"; exit 1; }

# run (ARK durable backend so >64 distinct blocks round-trip) ----------------
export PKERNEL_PFS_BACKEND=ark
export PKERNEL_ARK_IMG="$WORK/ark.img"
export PKERNEL_ARK_SECTORS=131072        # 64 MiB image (comfortably > ~2 MB S blob)

echo ""
echo "[run] SS-3 blob-transport in-process cert (SOLO, ARK-backed) ..."
"$WORK/blob"
CERT_RC=$?

echo ""
if [ "$CERT_RC" -eq 0 ]; then
    echo "[result] PASS"
    echo "[note] STEP 1 gate. The [live] 3-process relay harness (STEP 3) is a"
    echo "       DEFERRED row — but note its fetcher needs the SAME ARK backend:"
    echo "       a 482-chunk blob does not fit a memory-only 64-slot P0 store."
    exit 0
else
    echo "[result] FAIL (cert=$CERT_RC)"
    exit 1
fi
