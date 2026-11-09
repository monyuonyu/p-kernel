#!/bin/bash
# ===========================================================================
# run_ark_mix.sh — test-holes H22: each of the ark's two stops alone, with an old node.
#
# The ark stays on its node by two stops (ark_rec.c): pfs_dag neither beacons nor adopts
# an "ark/" name, and pfs_repl neither announces nor sends an "AKR" block. With both
# sides new, removing one stop is green (audit-169 K3/K6), because the other still holds.
# Here node A is an old-style build (ARK_MIX_OLD: -DARK_REC_SHARED, both stops off) and
# node B the build under test, so each stop of B is the only one in the way.
# The lines (fixed before the run, 2026-11-09):
#  x1  (session 1, B's receive stop alone) old A saves zz and writes a-one, so A beacons
#      ark/rec; then B writes b-one. Green iff B adopted zz (refs reach B), B adopted no 'ark/' ref, and B's
#      own record b-one is B's record 0 with nrec=1. (A second draft had B write first:
#      then the all-old negative stayed green on x1 - no tooth - so x1 and x2 are now
#      two sessions.)
#  x2  (session 2, B's send stop alone) a fresh B and an old node C with no records asks for B's AKRR by its
#      full id (`ark rec restore <id>` WANTs it without an announce). Green iff C never
#      logs that block as replicated and says the id is not in the region. (The first
#      draft asked from A, which has records, so restore refused before any WANT: a
#      green that tested nothing; seen in the first run.)
# Limit: B does store the old A's AKR blocks (A announces them): an old sender leaks its
# own records and nothing on the receiving side refuses an "AKR" block.
# Negative control: ARK_MIX_NEW=<another -DARK_REC_SHARED build> ARK_MIX_EXPECT_RED=1
# must make x1 and x2 red (the probe can see a leak).
# Usage: ARK_MIX_OLD=<-DARK_REC_SHARED build> tests/host/run_ark_mix.sh
# Output: [ark-mix] ALL PASS (0) / RED (1); with ARK_MIX_EXPECT_RED: NC RED as expected / NC FAIL.
# ===========================================================================
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
NEW="${ARK_MIX_NEW:-$ROOT/boot/linux_x86_64/p-kernel}"
OLD="${ARK_MIX_OLD:-}"
[ -x "$NEW" ] && [ -n "$OLD" ] && [ -x "$OLD" ] || { echo "[ark-mix] FAIL: need ARK_MIX_OLD (a -DARK_REC_SHARED build) and $NEW"; exit 1; }
d=$(mktemp -d); trap 'rm -rf "$d"' EXIT
RELAY="$ROOT/relay/relay"
[ -x "$RELAY" ] || make -s -C "$ROOT/relay" > /dev/null 2>&1
PORT="${ARK_MIX_PORT:-7747}"
export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
unset PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN
wait_for() { local n=0; while [ "$n" -lt "$(( $3 * 2 ))" ]; do grep -aqE "$2" "$1" 2>/dev/null && return 0; sleep 0.5; n=$((n + 1)); done; return 1; }
"$RELAY" -p "$PORT" > "$d/relay.log" 2>&1 & RP=$!
sleep 1
node() {  # <id> <tag> <bin>: reads d/f<tag>, logs d/l<tag>.log
    exec env PKERNEL_NODE_ID="$1" PKERNEL_AUTONET=1 PKERNEL_PFS_BACKEND=ark PKERNEL_ARK_IMG="$d/$2.img" \
        PKERNEL_ARK_SECTORS=65536 PKERNEL_PFS_DIR="$d/$2.dir" PKERNEL_RELAY="127.0.0.1:$PORT" \
        timeout 180 "$3" < "$d/f$2" > "$d/l$2.log" 2>&1
}
# ---- session 1 (x1): old A writes first, then B -------------------------------
mkfifo "$d/fa" "$d/fb"; mkdir -p "$d/a.dir" "$d/b.dir"
node 1 a "$OLD" & PA=$!; exec 7<>"$d/fa"
node 2 b "$NEW" & PB=$!; exec 8<>"$d/fb"
for t in a b; do wait_for "$d/l$t.log" 'P2 version DAG ready' 60; done
sleep 15
printf 'pfs save zz shared\n' >&7
printf 'ark rec new 1 a-one\n' >&7
mesh=0; wait_for "$d/lb.log" "ref 'zz' -> seq 1" 40 && mesh=1
sleep 6                                      # several 800 ms ref beacons of A's ark/rec
printf 'ark rec new 1 b-one\n' >&8
sleep 3
printf 'ark rec ls\nark rec get 0\n' >&8
sleep 3
kill "$PA" "$PB" 2>/dev/null; exec 7>&- 8>&-; wait "$PA" "$PB" 2>/dev/null
# ---- session 2 (x2): B writes, old C (no records, no A) WANTs it by id ---------
mkfifo "$d/fb2" "$d/fc"; mkdir -p "$d/b2.dir" "$d/c.dir"
node 2 b2 "$NEW" & PB=$!; exec 8<>"$d/fb2"
node 3 c "$OLD" & PC=$!; exec 9<>"$d/fc"
for t in b2 c; do wait_for "$d/l$t.log" 'P2 version DAG ready' 60; done
sleep 15
printf 'ark rec new 1 b-one\n' >&8
sleep 2
printf 'ark rec log 0\n' >&8
sleep 2
bid=$(tr -d '\r' < "$d/lb2.log" | grep -aoE 'log no=0 seq=1 .* id=[0-9a-f]{64}' | grep -oE '[0-9a-f]{64}$' | head -1)
printf 'ark rec restore %s\n' "${bid:-0}" >&9
wait_for "$d/lc.log" '\[ark-rec\] (restore|error)' 60
sleep 2
kill "$PB" "$PC" "$RP" 2>/dev/null; exec 8>&- 9>&-; wait "$PB" "$PC" "$RP" 2>/dev/null
for t in a b c; do tr -d '\r' < "$d/l$t.log" | sed 's/^\(p-kernel> \)*//' > "$d/$t.t"; done
bad=""
adopt=$(grep -acE "ref 'ark/" "$d/b.t")
if [ "$mesh" = 1 ] && [ "$adopt" = 0 ] && grep -aqE '^\[ark-rec\] index nrec=1 ' "$d/b.t" \
   && grep -aqE '^\[ark-rec\] get no=0 seq=1 .*: b-one$' "$d/b.t"; then
    echo "[ark-mix] ok   x1: the old A's refs reach B (zz), B adopts no ark/ ref, B's record 0 is b-one"
else echo "[ark-mix] bad  x1: mesh=$mesh ark/-adoptions=$adopt B[$(grep -aE '^\[ark-rec\]|ref .ark/' "$d/b.t" | head -6 | tr '\n' '|')]"; bad="$bad x1"; fi
got=1; [ -n "$bid" ] && ! grep -aqE "replicated block id=${bid:0:16}" "$d/c.t" && got=0
if [ -n "$bid" ] && [ "$got" = 0 ] && grep -aqE '^\[ark-rec\] restore error: the sealed index is not in the region' "$d/c.t"; then
    echo "[ark-mix] ok   x2: the old C WANTed B's record ${bid:0:16} and did not get it"
else echo "[ark-mix] bad  x2: id=${bid:-none} C-got=$got C[$(grep -aE '^\[ark-rec\] (restore|error)' "$d/c.t" | tr '\n' '|')]"; bad="$bad x2"; fi
if [ -n "${ARK_MIX_EXPECT_RED:-}" ]; then
    if [ "$bad" = " x1 x2" ]; then echo "[ark-mix] NC RED as expected"; exit 0; fi
    echo "[ark-mix] NC FAIL (red:$bad)"; exit 1
fi
if [ -z "$bad" ]; then echo "[ark-mix] ALL PASS"; exit 0; fi
echo "[ark-mix] RED:$bad"; exit 1
