#!/bin/bash
# ===========================================================================
# run_ark_full.sh — the ARK backend keeps taking p-fs blocks when its log fills
# with old commits (ark-entry.md 2.3: the product path that calls ark_compact).
#
# Every pfs_ark_put appends the block and a checkpoint commit; old commits are
# never reclaimed unless ark_compact runs, so a small image goes write-dead long
# before its live blocks fill it. Fixed before the code (2026-11-07):
#   [ark-full] PASS iff, on a PKERNEL_ARK_SECTORS=64 image, all ARK_FULL_N=20
#   distinct `pfs put`s succeed, a fresh process on the same image serves all 20
#   back (`pfs get`), and the writer printed at least one compaction line.
# master before the change: 10 of 20 go in (probe, pk-scratch/a147/ackp.sh).
# Negative control: a build with -DPFS_ARK_NO_AUTOCOMPACT must be [ark-full] FAIL.
# Record only: ARK_FULL_OVER=1 also puts 60 into the same size and prints how
# many went in and that the first ones still read back (no line judged).
#
# Usage: tests/host/run_ark_full.sh   (ARK_FULL_BIN: default boot/linux_x86_64/p-kernel)
# ===========================================================================
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BIN="${ARK_FULL_BIN:-$ROOT/boot/linux_x86_64/p-kernel}"
SEC="${ARK_FULL_SECTORS:-64}"
N="${ARK_FULL_N:-20}"
[ -x "$BIN" ] || { echo "[ark-full] FAIL: no binary $BIN"; exit 1; }
d=$(mktemp -d); trap 'rm -rf "$d"' EXIT

text() { printf 'arkfull-block-%04d-padpadpadpadpadpadpadpadpadpad' "$1"; }

run() {   # $1 image  $2 log  $3 put|get  $4 count
    {
        sleep 1
        for i in $(seq 1 "$4"); do printf 'pfs %s %s\n' "$3" "$(text "$i")"; sleep 0.15; done
        sleep 2
    } | env PKERNEL_PFS_BACKEND=ark PKERNEL_ARK_IMG="$1" PKERNEL_ARK_SECTORS="$SEC" \
          timeout 120 "$BIN" > "$2" 2>&1
    tr -d '\r' < "$2" > "$2.t" && mv "$2.t" "$2"
}

run "$d/a.img" "$d/w.log" put "$N"
ok=$(grep -ac '^\[pfs\] put len=' "$d/w.log")
comp=$(grep -ac '^\[pfs\] durable(ark): log full, compacted' "$d/w.log")
run "$d/a.img" "$d/r.log" get "$N"
got=0
for i in $(seq 1 "$N"); do grep -aqxF "[pfs] get: $(text "$i")" "$d/r.log" && got=$((got + 1)); done
nf=$(grep -aci 'not found' "$d/r.log")
echo "[ark-full] sectors=$SEC n=$N put_ok=$ok compactions=$comp remount_served=$got not_found=$nf"

if [ "${ARK_FULL_OVER:-0}" = 1 ]; then
    run "$d/b.img" "$d/w2.log" put 60
    ok2=$(grep -ac '^\[pfs\] put len=' "$d/w2.log")
    run "$d/b.img" "$d/r2.log" get 5
    got2=0; for i in 1 2 3 4 5; do grep -aqxF "[pfs] get: $(text "$i")" "$d/r2.log" && got2=$((got2 + 1)); done
    echo "[ark-full] record only: 60 into $SEC sectors -> put_ok=$ok2, first 5 served after remount: $got2"
fi

if [ "$ok" = "$N" ] && [ "$got" = "$N" ] && [ "$nf" = 0 ] && [ "$comp" -ge 1 ]; then
    echo "[ark-full] PASS (a full log is compacted on put; all $N blocks survive a remount)"
    exit 0
fi
echo "[ark-full] FAIL"
cp "$d/w.log" /tmp/ark_full_w.log 2>/dev/null; cp "$d/r.log" /tmp/ark_full_r.log 2>/dev/null
exit 1
