#!/bin/bash
# ===========================================================================
# run_ark_full.sh — the ARK backend keeps taking p-fs blocks when its log fills
# with old commits (ark-entry.md 2.3: the product path that calls ark_compact).
#
# Every pfs_ark_put appends the block and a checkpoint commit; old commits are
# never reclaimed unless ark_compact runs, so a small image goes write-dead long
# before its live blocks fill it.
#
# The line, and how it was set (2026-11-07, be honest about it):
#   The first line, fixed before the code, was 20 blocks into 64 sectors. It
#   cannot be met by any schedule: ark_compact writes a whole copy of the blocks
#   into free space, so the live blocks can never pass about half the image
#   (20 blocks ~ 42 sectors > 31). Measured: master 10/20, the change 18/20.
#   That arithmetic was not done before the line was set.
#   The line now, from that arithmetic (set AFTER seeing 18/20 at 64 sectors):
#   a case passes iff all N distinct `pfs put`s succeed, a fresh process on the
#   same image serves all N back (`pfs get`), and the writer printed at least
#   one compaction line. Cases: 64 sectors N=14 (2N+2 sectors <= 31) and
#   128 sectors N=28 (not run before this line was written). [ark-full] PASS
#   iff both pass.
# Negative control: a build with -DPFS_ARK_NO_AUTOCOMPACT must be [ark-full] FAIL.
# Record only: ARK_FULL_OVER=1 also puts 60 into 64 sectors and prints how many
# went in and that the first 5 still read back (no line judged).
#
# Usage: tests/host/run_ark_full.sh   (ARK_FULL_BIN: default boot/linux_x86_64/p-kernel)
# ===========================================================================
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BIN="${ARK_FULL_BIN:-$ROOT/boot/linux_x86_64/p-kernel}"
[ -x "$BIN" ] || { echo "[ark-full] FAIL: no binary $BIN"; exit 1; }
d=$(mktemp -d); trap 'rm -rf "$d"' EXIT

text() { printf 'arkfull-block-%04d-padpadpadpadpadpadpadpadpadpad' "$1"; }

run() {   # $1 image  $2 log  $3 put|get  $4 count  $5 sectors
    {
        sleep 1
        for i in $(seq 1 "$4"); do printf 'pfs %s %s\n' "$3" "$(text "$i")"; sleep 0.15; done
        sleep 2
        printf 'exit\n'     # the shell does not end on EOF; puts are fsync'd already
    } | env PKERNEL_PFS_BACKEND=ark PKERNEL_ARK_IMG="$1" PKERNEL_ARK_SECTORS="$5" \
          timeout 60 "$BIN" > "$2" 2>&1
    tr -d '\r' < "$2" > "$2.t" && mv "$2.t" "$2"
}

fails=0
case_() {   # $1 sectors  $2 n
    local sec=$1 n=$2 tag="$1-$2"
    run "$d/$tag.img" "$d/$tag-w.log" put "$n" "$sec"
    local ok comp got nf
    ok=$(grep -ac '^\[pfs\] put len=' "$d/$tag-w.log")
    comp=$(grep -acE '^\[pfs\] durable\(ark\): .*, compacted \(' "$d/$tag-w.log")
    run "$d/$tag.img" "$d/$tag-r.log" get "$n" "$sec"
    got=0
    for i in $(seq 1 "$n"); do grep -aqxF "[pfs] get: $(text "$i")" "$d/$tag-r.log" && got=$((got + 1)); done
    nf=$(grep -aci 'not found' "$d/$tag-r.log")
    if [ "$ok" = "$n" ] && [ "$got" = "$n" ] && [ "$nf" = 0 ] && [ "$comp" -ge 1 ]; then
        echo "[ark-full] ok   sectors=$sec n=$n put_ok=$ok compactions=$comp remount_served=$got"
    else
        echo "[ark-full] bad  sectors=$sec n=$n put_ok=$ok compactions=$comp remount_served=$got not_found=$nf"
        cp "$d/$tag-w.log" "/tmp/ark_full_$tag-w.log" 2>/dev/null
        fails=$((fails + 1))
    fi
}
case_ 64 14
case_ 128 28

if [ "${ARK_FULL_OVER:-0}" = 1 ]; then
    run "$d/o.img" "$d/o-w.log" put 60 64
    ok2=$(grep -ac '^\[pfs\] put len=' "$d/o-w.log")
    c2=$(grep -acE '^\[pfs\] durable\(ark\): .*, compacted \(' "$d/o-w.log")
    run "$d/o.img" "$d/o-r.log" get 5 64
    got2=0; for i in 1 2 3 4 5; do grep -aqxF "[pfs] get: $(text "$i")" "$d/o-r.log" && got2=$((got2 + 1)); done
    echo "[ark-full] record only: 60 into 64 sectors -> put_ok=$ok2 compactions=$c2, first 5 served after remount: $got2"
fi

if [ "$fails" = 0 ]; then
    echo "[ark-full] PASS (the log is compacted on put; every block survives a remount)"
    exit 0
fi
echo "[ark-full] FAIL"
exit 1
