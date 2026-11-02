#!/bin/bash
# tests/host/run_n2_narrow.sh — [n2-narrow] (ROADMAP N2, research/n2-wire16.md): on hosted a
# node id is 16 bits (pk_node_t), and nothing may cut one to a byte without saying so. A wide
# id going into a one-byte field goes through pk_node_v1(), which makes it "not a node"
# instead of an alias (300 would be 44). The compiler is the reader: this builds the hosted
# kernel with -Wconversion and is red on any conversion from pk_node_t to a byte, or on a
# pointer of the wrong width (a pk_node_t * written through a byte variable, or the reverse).
#
# The negative control runs first: tests/host/n2_narrow_nc.c holds one of each, and the same
# filter must find both, so a compiler that changes its wording cannot turn this green.
#
# Not seen (limits): an explicit cast such as (UB)id, a byte that holds an id from the start
# (it never was pk_node_t), and a conversion through int (a ?: of a node id and a constant).
#
#   N2_TREE  tree to build, default this repository (it runs make clean there).
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${N2_TREE:-$(cd "$HERE/../.." && pwd)}"
WORK="$(mktemp -d /tmp/n2narrow.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

narrow='conversion from .pk_node_t. .* to .(UB|U1|unsigned char|uint8_t|pk_node8_t|char|signed char|B).'
ptr='incompatible pointer'

gcc -c -D_TK_HOSTED_LIBC_ -Wconversion -Wno-sign-conversion -I "$ROOT/arch/common/include" \
    "$HERE/n2_narrow_nc.c" -o "$WORK/nc.o" 2> "$WORK/nc.log"
a="$(grep -cE "$narrow" "$WORK/nc.log")"
b="$(grep -cE "$ptr" "$WORK/nc.log")"
if [ "$a" != 1 ] || [ "$b" != 1 ]; then
    echo "[n2-narrow] RED: the negative control gave narrow=$a pointer=$b, wanted 1 and 1"
    cat "$WORK/nc.log"
    exit 1
fi
echo "[n2-narrow] negative control: narrow=$a pointer=$b"

make -C "$ROOT/boot/linux_x86_64" clean > /dev/null 2>&1
make -k -j"$(nproc)" -C "$ROOT/boot/linux_x86_64" \
    EXTRA_CFLAGS="-Wconversion -Wno-sign-conversion" > "$WORK/build.log" 2>&1
rc=$?
make -C "$ROOT/boot/linux_x86_64" clean > /dev/null 2>&1
if [ "$rc" != 0 ]; then
    echo "[n2-narrow] RED: the build failed (rc=$rc)"
    grep -E ' error: ' "$WORK/build.log" | head -20
    exit 1
fi
a="$(grep -cE "$narrow" "$WORK/build.log")"
b="$(grep -cE "$ptr" "$WORK/build.log")"
if [ "$a" != 0 ] || [ "$b" != 0 ]; then
    echo "[n2-narrow] RED: narrow=$a pointer=$b"
    grep -E "$narrow|$ptr" "$WORK/build.log" | sed 's,^\.\./\.\./,,' | cut -c1-140 | head -40
    exit 1
fi
echo "[n2-narrow] PASS: no node id cut to a byte, no pointer of the wrong width"
