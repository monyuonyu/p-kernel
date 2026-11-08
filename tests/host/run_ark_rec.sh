#!/bin/bash
# ===========================================================================
# run_ark_rec.sh — ark-entry.md 3 E1 (restart part), E2, E4: the ark's records
# and their index (`ark rec`, `ark heir`; arch/common/ark_rec.c, hosted only).
#
# The lines (fixed before the code, 2026-11-09):
#  E1  [ark-rec-e1] 100 records (record 1 is 10240 bytes, so 3 fragments) and
#      record 0 with 70 versions, index fanout 4 (PKERNEL_ARK_REC_FANOUT=4, so the
#      index has 4 levels), ARK backend. A fresh process on the same image gives
#      back every version of record 0 (text "v<k>" at @k, k=1..70), the sha256 of
#      record 1 equal to an independent python sha256 of the fill pattern, and
#      `ark rec ls` with nrec=100 and depth>=3, all equal to what the writer read.
#  E1c [ark-rec-stop] a process stopped (PKERNEL_ARK_REC_STOP=1: _exit after the
#      index blocks, before the self/rec version) during `ark rec set 0 c3` leaves
#      record 0 at seq 2 = "c2" with @1 = "c1" readable after a restart.
#  E2  [ark-rec-e2] old versions stay readable after ark_compact: a small image
#      (PKERNEL_ARK_SECTORS=ARK_REC_E2_SECTORS) and 24 versions of one record; the
#      writer prints at least one compaction line and a fresh process reads all 24.
#      (E1 also reads all 70 old versions.) mw_gc is not driven here: it drops only
#      blocks whose first 4 bytes are MW_MAGIC and whose length is a weight chunk's
#      or manifest's; the ark's blocks start with other magics (a limit, by reading).
#  E4  [ark-rec-e4] a new record is vis=self; `ark rec vis 0 heir` makes seq 2 with
#      vis=heir and seq 1 still self; `ark heir set <key>` then `ark heir clear` leave
#      two versions of self/heir (`ark heir log`: 1 set <first 8 hex>, 2 cleared),
#      and both survive a restart.
# Not here (ark-entry.md E1, later): the region kill of one node.
# Negative controls (each must make its line red; ARK_REC_EXPECT_RED=<line>):
#   -DARK_REC_OVERWRITE      e1 and e2 (a new version forgets its previous one)
#   -DARK_REC_DESC_FIRST     stop   (self/rec is written before the index blocks)
#   -DARK_REC_DEFAULT_PUBLIC e4     (a new record is public)
#
# Usage: tests/host/run_ark_rec.sh   (ARK_REC_BIN: default boot/linux_x86_64/p-kernel)
#        ARK_REC_EXPECT_RED="e1 e2" ARK_REC_BIN=<negative build> tests/host/run_ark_rec.sh
# Output: [ark-rec] ALL PASS (exit 0) / [ark-rec] RED (1); with ARK_REC_EXPECT_RED:
#         [ark-rec] NC RED as expected (0) / [ark-rec] NC FAIL (1).
# ===========================================================================
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BIN="${ARK_REC_BIN:-$ROOT/boot/linux_x86_64/p-kernel}"
E2SEC="${ARK_REC_E2_SECTORS:-160}"
[ -x "$BIN" ] || { echo "[ark-rec] FAIL: no binary $BIN"; exit 1; }
d=$(mktemp -d); trap 'rm -rf "$d"' EXIT

# run <tag> <sectors> <cmdfile> [env...]: one p-kernel on image d/<tag>.img, refs in d/<tag>.dir
run() {
    local tag=$1 sec=$2 cmds=$3; shift 3
    {
        sleep 1
        while IFS= read -r l; do printf '%s\n' "$l"; sleep 0.05; done < "$cmds"
        sleep 1
        printf 'exit\n'
    } | env PKERNEL_PFS_BACKEND=ark PKERNEL_ARK_IMG="$d/$tag.img" PKERNEL_ARK_SECTORS="$sec" \
          PKERNEL_PFS_DIR="$d/$tag.dir" PKERNEL_ARK_REC_FANOUT=4 "$@" \
          timeout 120 "$BIN" > "$d/out.log" 2>&1
    tr -d '\r' < "$d/out.log" | sed 's/^\(p-kernel> \)*//' >> "$d/$tag.log"
}
reads() { grep -aE '^\[ark-rec\] (get|sum|index|  no=)' "$1" | grep -av 'crash' ; }

bad=""
say() { echo "[ark-rec] $1"; }

# ---- E1 -------------------------------------------------------------------
c="$d/e1w.cmd"; : > "$c"
echo "ark rec new 1 v1" >> "$c"
echo "ark rec fill 2 10240 3" >> "$c"
for i in $(seq 2 99); do echo "ark rec new 1 r$i" >> "$c"; done
for k in $(seq 2 70); do echo "ark rec set 0 v$k" >> "$c"; done
q="$d/e1q.cmd"; : > "$q"
for k in $(seq 1 70); do echo "ark rec get 0 @$k" >> "$q"; done
echo "ark rec sum 1" >> "$q"; echo "ark rec get 99" >> "$q"; echo "ark rec ls" >> "$q"
cat "$c" "$q" > "$d/e1a.cmd"
run e1 65536 "$d/e1a.cmd"; cp "$d/e1.log" "$d/e1-w.log"; : > "$d/e1.log"
run e1 65536 "$q"; cp "$d/e1.log" "$d/e1-r.log"
want=$(python3 -c 'import hashlib;print(hashlib.sha256(bytes(((3+i*7+(i>>9)*13)&255) for i in range(10240))).hexdigest())')
ok=1; got=0
for k in $(seq 1 70); do grep -aqE "^\[ark-rec\] get no=0 seq=$k .*: v$k\$" "$d/e1-r.log" && got=$((got+1)); done
[ "$got" = 70 ] || ok=0
grep -aqE "^\[ark-rec\] sum no=1 seq=1 len=10240 sha256=$want\$" "$d/e1-r.log" || ok=0
grep -aqE '^\[ark-rec\] get no=99 seq=1 .*: r99$' "$d/e1-r.log" || ok=0
dep=$(grep -aoE '^\[ark-rec\] index nrec=100 depth=[0-9]+' "$d/e1-r.log" | grep -oE '[0-9]+$' | head -1)
[ -n "$dep" ] && [ "$dep" -ge 3 ] || ok=0
[ "$(reads "$d/e1-w.log" | tail -n "$(reads "$d/e1-r.log" | wc -l)")" = "$(reads "$d/e1-r.log")" ] || ok=0
nl=$(reads "$d/e1-r.log" | wc -l)
if [ "$ok" = 1 ]; then say "ok   e1 restart: 70/70 versions, 10240 B sha256 = python, nrec=100 depth=$dep, $nl read lines same as the writer"
else say "bad  e1 restart: versions $got/70 depth=${dep:-?} read lines $nl"; bad="$bad e1"; cp "$d/e1-r.log" /tmp/ark_rec_e1-r.log 2>/dev/null; fi

# ---- E1c: stop before the index version ------------------------------------
printf 'ark rec new 1 c1\nark rec set 0 c2\n' > "$d/s1.cmd"
printf 'ark rec set 0 c3\n' > "$d/s2.cmd"
printf 'ark rec get 0\nark rec get 0 @1\nark rec ls\n' > "$d/s3.cmd"
run st 65536 "$d/s1.cmd"; : > "$d/st.log"
run st 65536 "$d/s2.cmd" PKERNEL_ARK_REC_STOP=1; cp "$d/st.log" "$d/st-2.log"; : > "$d/st.log"
run st 65536 "$d/s3.cmd"
if grep -aq 'stop-test' "$d/st-2.log" && grep -aqE '^\[ark-rec\] get no=0 seq=2 .*: c2$' "$d/st.log" \
   && grep -aqE '^\[ark-rec\] get no=0 seq=1 .*: c1$' "$d/st.log" && grep -aqE '^\[ark-rec\] index nrec=1 ' "$d/st.log"; then
    say "ok   stop: stopped before the self/rec version, record 0 is seq 2 = c2 and @1 = c1"
else say "bad  stop: $(grep -aE '^\[ark-rec\] (get|index)' "$d/st.log" | tr '\n' '|')"; bad="$bad stop"; fi

# ---- E2: after ark_compact --------------------------------------------------
c="$d/e2w.cmd"; echo "ark rec new 1 w1" > "$c"
for k in $(seq 2 24); do echo "ark rec set 0 w$k" >> "$c"; done
q="$d/e2q.cmd"; : > "$q"; for k in $(seq 1 24); do echo "ark rec get 0 @$k" >> "$q"; done
run e2 "$E2SEC" "$c"; cp "$d/e2.log" "$d/e2-w.log"; : > "$d/e2.log"
run e2 "$E2SEC" "$q"
comp=$(grep -acE '^\[pfs\] durable\(ark\): .*, compacted \(' "$d/e2-w.log")
sets=$(grep -acE '^\[ark-rec\] (new|set) no=0 ' "$d/e2-w.log")
got=0; for k in $(seq 1 24); do grep -aqE "^\[ark-rec\] get no=0 seq=$k .*: w$k\$" "$d/e2.log" && got=$((got+1)); done
if [ "$comp" -ge 1 ] && [ "$sets" = 24 ] && [ "$got" = 24 ]; then
    say "ok   e2 compact: sectors=$E2SEC compactions=$comp, 24/24 versions after a restart"
else say "bad  e2 compact: sectors=$E2SEC compactions=$comp writes=$sets/24 versions=$got/24"; bad="$bad e2"; cp "$d/e2-w.log" /tmp/ark_rec_e2-w.log 2>/dev/null; fi

# ---- E4 ---------------------------------------------------------------------
K=00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff
printf 'ark rec new 1 e4\nark rec vis 0 heir\nark heir set %s\nark heir clear\n' "$K" > "$d/v1.cmd"
printf 'ark rec get 0 @1\nark rec get 0\nark heir log\n' > "$d/v2.cmd"
run e4 65536 "$d/v1.cmd"; cp "$d/e4.log" "$d/e4-w.log"; : > "$d/e4.log"
run e4 65536 "$d/v2.cmd"
if grep -aqE '^\[ark-rec\] new no=0 seq=1 vis=self ' "$d/e4-w.log" \
   && grep -aqE '^\[ark-rec\] get no=0 seq=1 vis=self .*: e4$' "$d/e4.log" \
   && grep -aqE '^\[ark-rec\] get no=0 seq=2 vis=heir .*: e4$' "$d/e4.log" \
   && grep -aqE '^\[ark-heir\] seq=1 set 00112233$' "$d/e4.log" \
   && grep -aqE '^\[ark-heir\] seq=2 cleared$' "$d/e4.log"; then
    say "ok   e4: new is vis=self, vis heir is seq 2 (seq 1 still self), heir set+clear are 2 versions after a restart"
else say "bad  e4: $(grep -aE '^\[ark-(rec|heir)\]' "$d/e4-w.log" "$d/e4.log" | tr '\n' '|')"; bad="$bad e4"; fi

# ---- verdict ----------------------------------------------------------------
if [ -n "${ARK_REC_EXPECT_RED:-}" ]; then
    want_red=$(echo $ARK_REC_EXPECT_RED | tr ' ' '\n' | sort | tr '\n' ' ')
    got_red=$(echo $bad | tr ' ' '\n' | sort | tr '\n' ' ')
    if [ "$want_red" = "$got_red" ]; then say "NC RED as expected ($got_red)"; exit 0; fi
    say "NC FAIL: wanted red [$want_red] got red [$got_red]"; exit 1
fi
if [ -z "$bad" ]; then say "ALL PASS"; exit 0; fi
say "RED:$bad"; exit 1
