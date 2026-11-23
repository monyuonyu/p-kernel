#!/bin/bash
# ---------------------------------------------------------------------------
# run_rg_fetch_tie.sh - [rg-fetch-tie] (research/b5-recv-mem.md §6): the round's
# fetch buffer ST_RG_FETCH_MAX (student.h) is written out as 8192 x 4096 + hdr;
# publish (gossip_learn.c) refuses a copy of more than GL_ST_MAXCHUNK chunks of
# GL_ST_CHUNK bytes. If one side changes alone, gl_student_fetch says "len > cap"
# and the round reads "could not fetch" (not a false green, but silent).
# gossip_learn.c (hosted) ties the two with _Static_assert.
#
#   exit 0 = the tree as is compiles, and each of the two one-sided changes
#            (made here with sed on a copy) is refused by the compiler with
#            the tie's message:
#     grow    GL_ST_MAXCHUNK 8192u -> 16384u       (gossip_learn.h)
#     shrink  ST_RG_FETCH_MAX (8192u * -> (4096u * (student.h)
#   and, with the tie cut out of the copy (cut*), the same two changes compile
#   (the refusal is the tie's, and the cut still finds exactly the assert).
# Seconds, no model. Every arm on both the x86_64 and aarch64 hosted defines.
# RG_TIE_NC=1 (falsifier): the tie is cut out of each copy first -> grow and
# shrink must then compile, so every "refused" arm goes red.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
TIE='RG-FETCH tie'
fail=0

arm() {  # arm <name> <expect: ok|refused> <a64|x86>
    local n="$1" want="$2"
    local AC="$WORK/$n/arch/common" AL="$ROOT/arch/linux" K="$ROOT/kernel/mtkernel3"
    local defs="$A64" ARCHI="-I$ROOT/boot/linux -I$AL/aarch64/include -I$AL/include -I$ROOT/arch/aarch64/include"
    if [ "$3" = x86 ]; then
        defs="$X86"; ARCHI="-I$ROOT/boot/linux_x86_64 -I$AL/x86_64/include -I$AL/include"
    fi
    local INCS="$ARCHI -I$AC/include/lp64 -I$AC/include -I$ROOT/relay -I$K/kernel/knlinc -I$K/kernel/tkernel \
        -I$K/include -I$K/include/tm -I$K/include/compat -I$K/config"
    # shellcheck disable=SC2086
    $CC -std=gnu11 -Wall -Wextra -fsyntax-only $defs $INCS "$AC/gossip_learn.c" > "$WORK/$n.log" 2>&1
    local rc=$?
    local said=0
    grep -q "$TIE" "$WORK/$n.log" && said=1
    if [ "$want" = ok ] && [ $rc -eq 0 ]; then
        echo "[rg-fetch-tie] $n PASS (compiles)"
    elif [ "$want" = refused ] && [ $rc -ne 0 ] && [ $said -eq 1 ]; then
        echo "[rg-fetch-tie] $n PASS (refused by the tie)"
    else
        echo "[rg-fetch-tie] $n FAIL (want $want, rc=$rc, tie message $said)"
        tail -5 "$WORK/$n.log"
        fail=1
    fi
}

CUT='/^_Static_assert((unsigned long long)ST_RG_FETCH_MAX/,/RG-FETCH tie: ST_RG/d'
copy() {  # copy <name> [cut]: arch/common into the work dir; cut = the tie removed
    mkdir -p "$WORK/$1/arch"
    cp -r "$ROOT/arch/common" "$WORK/$1/arch/common"
    if [ "${2:-}" = cut ] || [ "${RG_TIE_NC:-0}" = 1 ]; then
        mutate "$1" gossip_learn.c "$CUT"
    fi
}
mutate() {  # mutate <name> <file under arch/common> <sed expr>
    local f="$WORK/$1/arch/common/$2"
    cp "$f" "$f.orig"
    sed -i "$3" "$f"
    if cmp -s "$f" "$f.orig"; then
        echo "[rg-fetch-tie] $1 FAIL (the sed did not change $2: the arm would test nothing)"
        fail=1
    fi
}

X86="-D_APP_X86_64_ -D_APP_LINUX_ -D_TK_HOSTED_LIBC_ -D_LINUX_X86_64_ -DPK_NOSW_OFF"
A64="-D_APP_AARCH64_ -D_APP_LINUX_ -D_TK_HOSTED_LIBC_ -D_LINUX_AARCH64_ -DPK_NOSW_OFF"

copy plain
copy grow;   mutate grow   include/gossip_learn.h 's/define GL_ST_MAXCHUNK  8192u/define GL_ST_MAXCHUNK  16384u/'
copy shrink; mutate shrink llm/student.h        's/define ST_RG_FETCH_MAX (8192u \*/define ST_RG_FETCH_MAX (4096u */'
arm plain  ok      a64
arm grow   refused a64
arm shrink refused a64
arm plain  ok      x86
arm grow   refused x86
arm shrink refused x86   # audit-220 found the x86 shrink arm missing
# audit-222: the refusals above come from the tie and nothing else - with the
# tie cut out (the sed range), the same one-sided changes compile. If the
# assert's form changes so that the range no longer cuts exactly it (one line,
# another first line), these arms go red instead of the falsifier going quiet.
copy cutplain cut
copy cutgrow cut;   mutate cutgrow   include/gossip_learn.h 's/define GL_ST_MAXCHUNK  8192u/define GL_ST_MAXCHUNK  16384u/'
copy cutshrink cut; mutate cutshrink llm/student.h        's/define ST_RG_FETCH_MAX (8192u \*/define ST_RG_FETCH_MAX (4096u */'
for a in a64 x86; do
    arm cutplain  ok "$a"
    arm cutgrow   ok "$a"
    arm cutshrink ok "$a"
done
if [ $fail -eq 0 ]; then echo "[result] PASS  rg-fetch-tie"; exit 0; fi
echo "[result] FAIL  rg-fetch-tie"; exit 1
