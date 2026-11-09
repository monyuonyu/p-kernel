#!/bin/bash
# ===========================================================================
# run_pfs_burst.sh — gap-ledger PFS-REPL-WANT-DROP: a burst of puts reaches a peer whole.
#
# The line (fixed before the fix, 2026-11-09; N 30 -> 60 after 30 was green on master): relay + A(1) + B(2), in-memory p-fs.
# A puts 60 blocks, one per 10 ms (50 ms until audit-172) (`pfs put burst-<i>`). Green iff, 40 s later, B's
# `pfs ls` lists every one of A's 60 ids. Before the fix B's want table (8) drops the
# rest of the burst and nothing announces them again (B got 18 of 36 in the E3 test).
# Measured 2026-11-09 (container): master 45/60, 58/60 (no gave-up line: the missing
# ids never entered the table); with the fix 60/60 twice; -DPFS_WANT_NO_OVF 57/60.
# audit-172 (2026-11-09, container, under a build_all): at 50 ms the negative was green 1 of 3
# (56, 58, 60 of 60; master 57, 58) - CI runs it once, so it would go red about 1 run in 3.
# The gap is 10 ms since then (PFS_BURST_GAP): negative 18, 14, 18; fix 60, 60. At 0 ms the
# fix too held 37 and 40 (no table-full line, 3 gave-up lines): a faster burst is lost before
# the want table (the announce reader), a limit the ring does not cover.
# Negative control: -DPFS_WANT_NO_OVF (the old drop) must be red.
# PFS_BURST_GAP=0 (fix/pfs-ann-pace, 2026-11-09, container under a build_all): the ring alone
# held 40 of 60, B's K-DDS kept queue (32, shared by topics) lost 20 announces (probe), and
# B gave up on 2 because a holder read 1 WANT a tick. With the announces paced 8 a tick at
# the sender, every ANNOUNCE and WANT of a tick read (up to 32): 60/60. Negatives at 0 ms:
# -DPFSR_NC_ANN_UNPACED (send at once) and -DPFSR_NC_WANT_LATEST (the old WANT read).
# Usage: tests/host/run_pfs_burst.sh  (PFS_BURST_BIN: default boot/linux_x86_64/p-kernel)
#        PFS_BURST_EXPECT_RED=1 ... for the negative build.
# Output: [pfs-burst] PASS (0) / FAIL (1); with PFS_BURST_EXPECT_RED: NC RED as expected / NC FAIL.
# ===========================================================================
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BIN="${PFS_BURST_BIN:-$ROOT/boot/linux_x86_64/p-kernel}"
N="${PFS_BURST_N:-60}"
[ -x "$BIN" ] || { echo "[pfs-burst] FAIL: no binary $BIN"; exit 1; }
d=$(mktemp -d); trap 'rm -rf "$d"' EXIT
RELAY="$ROOT/relay/relay"
[ -x "$RELAY" ] || make -s -C "$ROOT/relay" > /dev/null 2>&1
PORT="${PFS_BURST_PORT:-7745}"
export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
unset PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN PKERNEL_PFS_DIR PKERNEL_PFS_BACKEND
wait_for() { local n=0; while [ "$n" -lt "$(( $3 * 2 ))" ]; do grep -aqE "$2" "$1" 2>/dev/null && return 0; sleep 0.5; n=$((n + 1)); done; return 1; }
"$RELAY" -p "$PORT" > "$d/relay.log" 2>&1 & RP=$!
sleep 1
mkfifo "$d/fa" "$d/fb"
node() { exec env PKERNEL_NODE_ID="$1" PKERNEL_AUTONET=1 PKERNEL_RELAY="127.0.0.1:$PORT" timeout 150 "$BIN" < "$3" > "$d/l$2.log" 2>&1; }
node 1 a "$d/fa" & PA=$!; exec 7<>"$d/fa"
node 2 b "$d/fb" & PB=$!; exec 8<>"$d/fb"
wait_for "$d/la.log" 'P1 replication ready' 60; wait_for "$d/lb.log" 'P1 replication ready' 60
sleep 15
GAP="${PFS_BURST_GAP:-0.01}"
for i in $(seq 1 "$N"); do printf 'pfs put burst-%d\n' "$i" >&7; sleep "$GAP"; done
sleep 40
printf 'pfs ls\n' >&8
sleep 3
kill "$PA" "$PB" "$RP" 2>/dev/null; exec 7>&- 8>&-; wait "$PA" "$PB" "$RP" 2>/dev/null
tr -d '\r' < "$d/la.log" | sed 's/^\(p-kernel> \)*//' > "$d/a.t"
tr -d '\r' < "$d/lb.log" | sed 's/^\(p-kernel> \)*//' > "$d/b.t"
ids=$(grep -aoE '^\[pfs\] put len=[0-9]+  id=[0-9a-f]{16}' "$d/a.t" | grep -oE '[0-9a-f]{16}$')
nput=$(echo "$ids" | grep -c .)
got=0; for i in $ids; do grep -aqE "^  $i  len=" "$d/b.t" && got=$((got + 1)); done
ovf=$(grep -acE "^\[pfs\] want table full" "$d/b.t"); gave=$(grep -acE "^\[pfs\] gave up" "$d/b.t")
if [ "$nput" = "$N" ] && [ "$got" = "$N" ]; then res=PASS; else res=FAIL; fi
echo "[pfs-burst] A put $nput, B holds $got of them (want-table-full lines on B: $ovf, gave-up lines on B: $gave)"
if [ -n "${PFS_BURST_EXPECT_RED:-}" ]; then
    if [ "$res" = FAIL ] && [ "$nput" = "$N" ]; then echo "[pfs-burst] NC RED as expected"; exit 0; fi
    echo "[pfs-burst] NC FAIL"; exit 1
fi
echo "[pfs-burst] $res"; [ "$res" = PASS ]
