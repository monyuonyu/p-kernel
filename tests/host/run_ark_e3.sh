#!/bin/bash
# ===========================================================================
# run_ark_e3.sh — ark-entry.md 5, E3 (sealed copies leave the node) and E1's region kill.
#
# The lines (fixed before the code, 2026-11-09 19:1x; ark-entry.md 5 "合否"):
#  e3a  relay + A(1) + B(2) + later C(3), D(4); ARK backend each, index fanout 4. A writes
#       record 0 = 10240 B (fill seed 3) and 2 more versions (seeds 5, 7), record 1 = a
#       text with a 24-byte marker, record 2 = a short text then `vis 2 public` (public
#       is sealed too, audit-170 2). Green iff B's ARK image holds the four sealed magics
#       AKSR AKSF AKSI AKSD, none of AKRR AKRF AKRI AKRD, no 16-byte window of any of
#       the three fill bodies, no marker; and A's own image (the positive control of the
#       scan) holds AKRR and the marker.
#  e3k  `ark rec keys 0` on A: three versions, three different key fingerprints (one key
#       never seals two plaintexts, the audit-158 note).
#  e3d  (E1's region kill) A runs `ark rec sync` (announces every sealed block again,
#       paced); after B lists A's last sealed= id, A is killed -9. C (a fresh
#       node with a copy of A's ark.key) runs `ark rec restore <sealed id>`: its `ark rec
#       ls` equals A's last, and the sums of record 0 @1 @2 @3, record 1 and record 2
#       equal A's (and @1 equals python's sha256 of the fill).
#  e3c  D (a fresh node with its own new ark.key) cannot restore the same id: it says
#       "cannot open" and lists no record.
#  e3b  A restarted alone on its image: `ark rec sget 0 @1..3` (read through the sealed
#       copy) give A's sums; with PKERNEL_ARK_REC_TAMPER=1 (one bit of the fragment
#       ciphertext flipped after it is read) sget says "cannot open" and prints no body.
#  e3f  without PKERNEL_ARK_REC_FANOUT a new index has fanout 124 (= the sealed index's
#       block, audit-170 1).
# Negative controls (ARK_E3_EXPECT_RED = the lines that must be red, no others):
# measured 2026-11-09 in the container, each NC RED as expected:
#   -DARK_REC_PLAIN        e3a e3b e3d   fragments carry the plaintext (B: 60 body windows, marker)
#   -DARK_REC_SAME_KEY     e3k           one key across versions (3 versions, 1 key)
#   -DARK_REC_INDEX_LOCAL  e3a e3b e3c e3d e3k   no AKSI / AKSD (nothing to restore from)
#   -DARK_REC_AAD_INNER    e3b e3d       the AAD holds the inner kind (the first draft's form)
# Usage: tests/host/run_ark_e3.sh  (ARK_E3_BIN: default boot/linux_x86_64/p-kernel)
# Output: [ark-e3] ALL PASS (0) / RED (1); with ARK_E3_EXPECT_RED: NC RED as expected / NC FAIL.
# ===========================================================================
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BIN="${ARK_E3_BIN:-$ROOT/boot/linux_x86_64/p-kernel}"
[ -x "$BIN" ] || { echo "[ark-e3] FAIL: no binary $BIN"; exit 1; }
d=$(mktemp -d); trap 'if [ -n "${ARK_E3_KEEP:-}" ]; then cp -r "$d" "$ARK_E3_KEEP"; fi; rm -rf "$d"' EXIT
bad=""
say() { echo "[ark-e3] $1"; }
clean() { tr -d '\r' < "$1" | sed 's/^\(p-kernel> \)*//'; }
wait_for() { local n=0; while [ "$n" -lt "$(( $3 * 2 ))" ]; do grep -aqE "$2" "$1" 2>/dev/null && return 0; sleep 0.5; n=$((n + 1)); done; return 1; }
MARK=E3MARK-q7Zp2Lx9Vw4Kc8Rt

RELAY="$ROOT/relay/relay"
[ -x "$RELAY" ] || make -s -C "$ROOT/relay" > /dev/null 2>&1
PORT="${ARK_E3_PORT:-7741}"
export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
unset PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN
"$RELAY" -p "$PORT" > "$d/relay.log" 2>&1 & RP=$!
sleep 1
node() {  # <id> <tag> [env...]: reads d/f<tag>, logs d/l<tag>.log
    local id=$1 t=$2; shift 2
    exec env PKERNEL_NODE_ID="$id" PKERNEL_AUTONET=1 PKERNEL_PFS_BACKEND=ark PKERNEL_ARK_IMG="$d/l$t.img" \
        PKERNEL_ARK_SECTORS=65536 PKERNEL_PFS_DIR="$d/l$t.dir" PKERNEL_RELAY="127.0.0.1:$PORT" \
        PKERNEL_ARK_REC_FANOUT=4 "$@" timeout 240 "$BIN" < "$d/f$t" > "$d/l$t.log" 2>&1
}
mkdir -p "$d/la.dir" "$d/lb.dir" "$d/lc.dir" "$d/ld.dir"
mkfifo "$d/fa" "$d/fb" "$d/fc" "$d/fd"
node 1 a & PA=$!; exec 7<>"$d/fa"
node 2 b & PB=$!; exec 8<>"$d/fb"
wait_for "$d/la.log" 'P2 version DAG ready' 60; wait_for "$d/lb.log" 'P2 version DAG ready' 60
sleep 12
{
  echo "ark rec fill 2 10240 3"; echo "ark rec setfill 0 10240 5"; echo "ark rec setfill 0 10240 7"
  echo "ark rec new 1 the marker $MARK is here"
  echo "ark rec new 1 third-one"; echo "ark rec vis 2 public"
  echo "ark rec keys 0"
  for k in 1 2 3; do echo "ark rec sum 0 @$k"; done
  echo "ark rec sum 1"; echo "ark rec sum 2"; echo "ark rec ls"
} > "$d/a1.cmd"
while IFS= read -r l; do printf '%s\n' "$l" >&7; sleep 0.3; done < "$d/a1.cmd"
wait_for "$d/la.log" 'index nrec=3 .*sealed=' 30
printf 'ark rec sync\n' >&7                 # a burst of writes outruns B's want table (8)
wait_for "$d/la.log" '\[ark-rec\] sync (done|:)' 120
sealed=$(clean "$d/la.log" | grep -aoE '^\[ark-rec\] index nrec=3 .*sealed=[0-9a-f]{64}' | grep -oE '[0-9a-f]{64}$' | tail -1)
held=0
if [ -n "$sealed" ]; then
    for i in $(seq 1 30); do
        printf 'pfs ls\n' >&8; sleep 2
        clean "$d/lb.log" | grep -aqE "^  ${sealed:0:16}  len=" && { held=1; break; }
    done
fi
sleep 6                                     # the blocks written before the AKSD are older
ka=$(ps -o pid= --ppid "$PA" 2>/dev/null); kill -9 $ka "$PA" 2>/dev/null; exec 7>&-; wait "$PA" 2>/dev/null
clean "$d/la.log" > "$d/la1.t"
cp "$d/la.dir/ark.key" "$d/lc.dir/ark.key" 2>/dev/null
node 3 c & PC=$!; exec 9<>"$d/fc"
node 4 d & PD=$!; exec 6<>"$d/fd"
wait_for "$d/lc.log" 'P2 version DAG ready' 60; wait_for "$d/ld.log" 'P2 version DAG ready' 60
sleep 12
printf 'ark rec restore %s\n' "${sealed:-0}" >&9
printf 'ark rec restore %s\n' "${sealed:-0}" >&6
wait_for "$d/lc.log" '^(p-kernel> )*\[ark-rec\] restore (done|error)|\[ark-rec\] error' 90
wait_for "$d/ld.log" '\[ark-rec\] (restore|error)' 90
{ for k in 1 2 3; do echo "ark rec sum 0 @$k"; done; echo "ark rec sum 1"; echo "ark rec sum 2"; echo "ark rec ls"; } > "$d/q.cmd"
while IFS= read -r l; do printf '%s\n' "$l" >&9; printf '%s\n' "$l" >&6; sleep 0.3; done < "$d/q.cmd"
sleep 3
kill "$PB" "$PC" "$PD" "$RP" 2>/dev/null; exec 8>&- 9>&- 6>&-; wait "$PB" "$PC" "$PD" "$RP" 2>/dev/null
for t in b c d; do clean "$d/l$t.log" > "$d/l$t.t"; done

# ---- e3a ----------------------------------------------------------------------
scan=$(python3 - "$d/lb.img" "$d/la.img" "$MARK" <<'PY'
import sys
b = open(sys.argv[1], 'rb').read(); a = open(sys.argv[2], 'rb').read(); mark = sys.argv[3].encode()
def fill(seed, n=10240): return bytes(((seed + i*7 + (i >> 9)*13) & 255) for i in range(n))
out = []
out.append('sealed=' + ','.join(m for m in ('AKSR','AKSF','AKSI','AKSD') if m.encode() in b))
out.append('plainB=' + ','.join(m for m in ('AKRR','AKRF','AKRI','AKRD') if m.encode() in b))
hits = 0
for s in (3, 5, 7):
    f = fill(s)
    for off in range(0, len(f) - 16, 512):
        if f[off:off+16] in b: hits += 1
out.append('bodyB=%d' % hits)
out.append('markB=%d' % (mark in b))
out.append('markA=%d akrrA=%d' % (mark in a, b'AKRR' in a))
print(' '.join(out))
PY
)
if [ "$scan" = "sealed=AKSR,AKSF,AKSI,AKSD plainB= bodyB=0 markB=0 markA=1 akrrA=1" ]; then say "ok   e3a: B holds sealed blocks only ($scan)"
else say "bad  e3a: $scan"; bad="$bad e3a"; fi

# ---- e3k ----------------------------------------------------------------------
fps=$(grep -aoE '^\[ark-rec\] key no=0 seq=[0-9]+ fp=[0-9a-f]{16}' "$d/la1.t" | grep -oE '[0-9a-f]{16}$' | sort -u | wc -l)
nk=$(grep -acE '^\[ark-rec\] key no=0 seq=' "$d/la1.t")
if [ "$nk" = 3 ] && [ "$fps" = 3 ]; then say "ok   e3k: 3 versions, 3 different keys"
else say "bad  e3k: versions=$nk distinct=$fps"; bad="$bad e3k"; fi

# ---- e3d ----------------------------------------------------------------------
sums() { grep -aE '^\[ark-rec\] (sum no=|index nrec=|  no=)' "$1" | sed 's/ sealed=[0-9a-f]*//'; }
want=$(python3 -c 'import hashlib;print(hashlib.sha256(bytes(((3+i*7+(i>>9)*13)&255) for i in range(10240))).hexdigest())')
sa=$(sums "$d/la1.t" | tail -n 9); sc=$(sums "$d/lc.t" | tail -n 9)
if [ "$held" = 1 ] && [ -n "$sa" ] && [ "$sa" = "$sc" ] && echo "$sc" | grep -q "seq=1 len=10240 sha256=$want" \
   && [ "$(echo "$sc" | grep -c 'sum no=')" = 5 ]; then
    say "ok   e3d: A killed after B held its last sealed id; C restored 3 records (record 0: 3 versions) equal to A"
else say "bad  e3d: B-held=$held sealed=${sealed:0:16} A[$(echo "$sa" | tr '\n' '|')] C[$(echo "$sc" | tr '\n' '|')] C-says[$(grep -aE '^\[ark-rec\] (restore|error)' "$d/lc.t" | tr '\n' '|')]"; bad="$bad e3d"; fi

# ---- e3c ----------------------------------------------------------------------
if grep -aqE '^\[ark-rec\] restore .*cannot open' "$d/ld.t" && ! grep -aqE '^\[ark-rec\] (sum no=[0-9]+ seq=|  no=)' "$d/ld.t"; then
    say "ok   e3c: another ark.key cannot open the sealed id"
else say "bad  e3c: D[$(grep -aE '^\[ark-rec\]' "$d/ld.t" | head -8 | tr '\n' '|')]"; bad="$bad e3c"; fi

# ---- e3b: A again, alone on its image ----------------------------------------
runa() {  # <cmdfile> [env...]
    local c=$1; shift
    { sleep 1; while IFS= read -r l; do printf '%s\n' "$l"; sleep 0.05; done < "$c"; sleep 1; printf 'exit\n'; } \
      | env PKERNEL_PFS_BACKEND=ark PKERNEL_ARK_IMG="$d/la.img" PKERNEL_ARK_SECTORS=65536 PKERNEL_PFS_DIR="$d/la.dir" \
            "$@" timeout 120 "$BIN" 2>&1 | tr -d '\r' | sed 's/^\(p-kernel> \)*//'
}
printf 'ark rec sget 0 @1\nark rec sget 0 @2\nark rec sget 0 @3\nark rec sum 0 @1\nark rec sum 0 @2\nark rec sum 0 @3\n' > "$d/b.cmd"
runa "$d/b.cmd" > "$d/b.t"
printf 'ark rec sget 0\n' > "$d/t.cmd"
runa "$d/t.cmd" PKERNEL_ARK_REC_TAMPER=1 > "$d/t.t"
s1=$(grep -aE '^\[ark-rec\] sget no=0 seq=' "$d/b.t" | sed 's/ sget / sum /')
s2=$(grep -aE '^\[ark-rec\] sum no=0 seq=' "$d/la1.t" | tail -n 3)
if [ -n "$s1" ] && [ "$s1" = "$s2" ] && grep -aqE '^\[ark-rec\] sget no=0 cannot open' "$d/t.t" && ! grep -aqE '^\[ark-rec\] sget no=0 seq=' "$d/t.t"; then
    say "ok   e3b: after a restart sget gives the 3 versions' sums; one flipped bit -> cannot open, nothing printed"
else say "bad  e3b: sget[$(echo "$s1" | tr '\n' '|')] sum[$(echo "$s2" | tr '\n' '|')] tamper[$(grep -aE '^\[ark-rec\]' "$d/t.t" | tr '\n' '|')]"; bad="$bad e3b"; fi

# ---- e3f ----------------------------------------------------------------------
mkdir -p "$d/f.dir"; printf 'ark rec new 1 f\nark rec ls\n' > "$d/f.cmd"
fo=$( { sleep 1; while IFS= read -r l; do printf '%s\n' "$l"; sleep 0.05; done < "$d/f.cmd"; sleep 1; printf 'exit\n'; } \
      | env PKERNEL_PFS_BACKEND=ark PKERNEL_ARK_IMG="$d/f.img" PKERNEL_ARK_SECTORS=4096 PKERNEL_PFS_DIR="$d/f.dir" \
            timeout 60 "$BIN" 2>&1 | tr -d '\r' | grep -aoE 'index nrec=1 depth=1 fanout=[0-9]+' | grep -oE '[0-9]+$')
if [ "$fo" = 124 ]; then say "ok   e3f: the default fanout is 124"
else say "bad  e3f: fanout=${fo:-?}"; bad="$bad e3f"; fi

if [ -n "${ARK_E3_EXPECT_RED:-}" ]; then
    want_red=$(echo $ARK_E3_EXPECT_RED | tr ' ' '\n' | sort | tr '\n' ' ')
    got_red=$(echo $bad | tr ' ' '\n' | sort | tr '\n' ' ')
    if [ "$want_red" = "$got_red" ]; then say "NC RED as expected ($got_red)"; exit 0; fi
    say "NC FAIL: wanted red [$want_red] got red [$got_red]"; exit 1
fi
if [ -z "$bad" ]; then say "ALL PASS"; exit 0; fi
say "RED:$bad"; exit 1
