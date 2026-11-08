#!/bin/bash
# ===========================================================================
# run_self_ref_local.sh — one node's consent must not open another node's gate
# (ark-profile.md 7.3; gap-ledger SELF-REF-REGION-SHARED).
#
# p-fs refs are region-wide and the newer seq wins, so node B adopted node A's
# self/prof (and self/lin, self/sig, self/prov) as its own: after A consented, B -
# which never read the manifesto - took /teach (200), and B's /profile.json showed
# A's profile. Probe 2026-11-09 (pk-scratch/arkrec-probe3.sh).
#
# The lines (fixed before the fix, 2026-11-09):
#   S0 precondition: B's /teach before anything is 403 "manifesto"; A's POST
#      /profile (ack, the served manifesto id) is 200; A's shared name zz reaches B
#      (B logs ref 'zz' adopted), so the refs do travel between them.
#   S1 after A's consent, B's /teach is still 403 "manifesto"
#   S2 B's /profile.json does not carry A's profile id
#   S3 no ref 'self/...' is adopted by B
# Negative control: -DSELF_REF_SHARED (the old shared self/ names) must redden S1-S3.
#
# Usage: tests/host/run_self_ref_local.sh   (SELF_REF_BIN: default boot/linux_x86_64/p-kernel)
#        SELF_REF_EXPECT_RED=1 ... : "[self-ref] NC RED as expected" iff S1, S2 and S3 are red
# Output: [self-ref] ALL PASS (exit 0) / [self-ref] RED (1). ~60 s.
# ===========================================================================
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BIN="${SELF_REF_BIN:-$ROOT/boot/linux_x86_64/p-kernel}"
[ -x "$BIN" ] || { echo "[self-ref] FAIL: no binary $BIN"; exit 1; }
RELAY="$ROOT/relay/relay"
[ -x "$RELAY" ] || make -s -C "$ROOT/relay" > /dev/null 2>&1
d=$(mktemp -d); RP=0; PA=0; PB=0
cleanup() { for p in $PA $PB $RP; do [ "$p" != 0 ] && kill "$p" 2>/dev/null; done; exec 7>&- 2>/dev/null; wait 2>/dev/null; rm -rf "$d"; }
trap cleanup EXIT
export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
unset PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN PKERNEL_GALAXY_PORT
PORT="${SELF_REF_PORT:-7737}"
wait_for() { local n=0; while [ "$n" -lt "$(( $3 * 2 ))" ]; do grep -aqE "$2" "$1" 2>/dev/null && return 0; sleep 0.5; n=$((n + 1)); done; return 1; }
"$RELAY" -p "$PORT" > "$d/relay.log" 2>&1 & RP=$!
sleep 1
mkfifo "$d/fa"
env PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$d/da" PKERNEL_RELAY="127.0.0.1:$PORT" \
    timeout 150 "$BIN" < "$d/fa" > "$d/a.log" 2>&1 & PA=$!; exec 7<>"$d/fa"
env PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$d/db" PKERNEL_RELAY="127.0.0.1:$PORT" \
    timeout 150 "$BIN" < /dev/null > "$d/b.log" 2>&1 & PB=$!
wait_for "$d/a.log" 'P2 version DAG ready' 60; wait_for "$d/b.log" 'P2 version DAG ready' 60
sleep 15
before=$(curl -s --max-time 8 -d 'k=sun&v=yellow' 127.0.0.1:7801/teach)
MID=$(curl -s -D - -o /dev/null --max-time 8 127.0.0.1:7800/manifesto | tr -d '\r' | grep -i 'X-Manifesto-Id' | awk '{print $2}')
prof=$(curl -s -w ' %{http_code}' --max-time 8 -d "ack=1&mid=$MID&handle=alice" 127.0.0.1:7800/profile)
pid=$(echo "$prof" | grep -oE '"id":"[0-9a-f]{64}"' | grep -oE '[0-9a-f]{64}')
printf 'pfs save zz shared\n' >&7
mesh=0; wait_for "$d/b.log" "ref 'zz' -> seq 1" 40 && mesh=1
sleep 6                                     # several 800 ms ref beacons after zz
after=$(curl -s --max-time 8 -d 'k=sun&v=yellow' 127.0.0.1:7801/teach)
bprof=$(curl -s --max-time 8 127.0.0.1:7801/profile.json)
adopt=$(tr -d '\r' < "$d/b.log" | grep -acE "ref 'self/")
echo "[self-ref] B before: $before | A profile: ${prof:0:40}... | mesh=$mesh | B after: $after | B self/ adoptions: $adopt"
reds=""
if echo "$before" | grep -q '"refused":"manifesto"' && echo "$prof" | grep -q ' 200$' && [ -n "$pid" ] && [ "$mesh" = 1 ]; then
    echo "[self-ref] ok   S0 precondition (B refused before, A consented, zz reached B)"
else echo "[self-ref] bad  S0 precondition"; reds="$reds S0"; fi
if echo "$after" | grep -q '"refused":"manifesto"'; then echo "[self-ref] ok   S1 B's gate stays closed after A's consent"
else echo "[self-ref] bad  S1 B took /teach after A's consent: $after"; reds="$reds S1"; fi
if [ -n "$pid" ] && ! echo "$bprof" | grep -q "$pid"; then echo "[self-ref] ok   S2 B's profile.json does not carry A's profile"
else echo "[self-ref] bad  S2 B's profile.json: ${bprof:0:200}"; reds="$reds S2"; fi
if [ "$adopt" = 0 ]; then echo "[self-ref] ok   S3 B adopted no self/ ref"
else echo "[self-ref] bad  S3 B adopted $adopt self/ ref(s)"; reds="$reds S3"; fi
if [ -n "${SELF_REF_EXPECT_RED:-}" ]; then
    if [ "$reds" = " S1 S2 S3" ]; then echo "[self-ref] NC RED as expected:$reds"; exit 0; fi
    echo "[self-ref] NC FAIL: red [$reds]"; exit 1
fi
if [ -z "$reds" ]; then echo "[self-ref] ALL PASS"; exit 0; fi
echo "[self-ref] RED:$reds"; exit 1
