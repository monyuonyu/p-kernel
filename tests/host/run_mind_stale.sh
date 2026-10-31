#!/bin/bash
# tests/host/run_mind_stale.sh — [mind-stale] (ROADMAP 1-3m, the model's STALE).
#
# The TLA+ model (tests/model/mind_rule/, run Sc-stale-L1A-fix) found a node
# that keeps a losing value for good, even with the 1-3m fix and a pull that
# is always answered: it held the old value, missed the winning teach, and the
# winner's teacher went on to teach another key, so its re-drive no longer
# carries the winner. Every re-send of the old value reuses a seq the others
# already took (per-origin once-per-seq), so nobody pushes back; and mind/have
# carries the winner's stamp, but a node only wants a key it never held
# (r3_want_hint), so it never asks.
#
# The same on two hosted nodes (built with -DMT_RX_INJECT):
#   B (id 2) gets "sky blue" from a node that is not there (node 5, stamp 1)
#   B is stopped at its console (`stop now`: it refuses network teaches and
#   does not use up their seq = they are lost to B, like a drop)
#   A (id 1) teaches fire warm, sky green, sea deep (stamps 1, 2, 3: sky green
#   at 2 beats B's blue at 1); then B `stop resume`
# Expectations fixed before the first run (on master 45f9d3fb):
#   S0 the hook works: B took the injected sky blue
#   S1 B refused A's teaches while stopped
#   S2 control: B gets fire by mind/have -> want -> pull (never held there)
#   S3 control: B gets sea from A's re-drive (A's last teach)
#   S4 B's sky becomes green (A's, the rule's answer) within 60 s of S2/S3
# Today S4 is expected RED (the hole) and S0-S3 green.
# Output "[mind-stale] <check> PASS|FAIL", then ALL PASS (exit 0) or RED
# (exit 1). Env: MST_PORT (default 7497), EXTRA_CFLAGS (added to
# -DMT_RX_INJECT). Rebuilds boot/linux* and cleans it afterwards.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    *)             BOOT="$ROOT/boot/linux_x86_64" ;;
esac
PORT="${MST_PORT:-7497}"
WORK="$(mktemp -d /tmp/mst.XXXXXX)"
REDS=""; RELAY=0; PA=0; PB=0
res() { echo "[mind-stale] $1 $2"; [ "$2" = FAIL ] && REDS="$REDS $1"; return 0; }
cleanup() {
    for p in "$PA" "$PB" "$RELAY"; do [ "$p" != 0 ] && kill -9 "$p" 2>/dev/null; done
    exec 7>&- 8>&- 2>/dev/null; wait 2>/dev/null
    make -C "$BOOT" clean >/dev/null 2>&1
    rm -rf "$WORK"/da "$WORK"/db
}
trap cleanup EXIT
wait_for() {  # <file> <ERE> <secs>
    local n=0
    while [ "$n" -lt "$(( $3 * 2 ))" ]; do
        grep -aqE "$2" "$1" 2>/dev/null && return 0
        sleep 0.5; n=$((n + 1))
    done
    return 1
}

make -C "$ROOT/relay" >/dev/null || exit 1
make -C "$BOOT" clean >/dev/null 2>&1
make -C "$BOOT" EXTRA_CFLAGS="-DMT_RX_INJECT ${EXTRA_CFLAGS:-}" > "$WORK/build.log" 2>&1 \
    || { echo "[mind-stale] BUILD FAILED"; tail -20 "$WORK/build.log"; exit 1; }

export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
export PKERNEL_GALAXY=0
unset PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN
"$ROOT/relay/relay" -p "$PORT" > "$WORK/relay.log" 2>&1 &
RELAY=$!
sleep 1
A="$WORK/a.log"; B="$WORK/b.log"
mkfifo "$WORK/fa" "$WORK/fb"; mkdir -p "$WORK/da" "$WORK/db"
env PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$WORK/da" PKERNEL_RELAY="127.0.0.1:$PORT" \
    "$BOOT/p-kernel" < "$WORK/fa" > "$A" 2>&1 &
PA=$!; exec 7<>"$WORK/fa"
env PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$WORK/db" PKERNEL_RELAY="127.0.0.1:$PORT" \
    "$BOOT/p-kernel" < "$WORK/fb" > "$B" 2>&1 &
PB=$!; exec 8<>"$WORK/fb"
wait_for "$A" 'mind_net_task up' 90 || echo "[mind-stale] note: A mind_net_task not seen"
wait_for "$B" 'mind_net_task up' 90 || echo "[mind-stale] note: B mind_net_task not seen"

printf 'mind inject 5 1 sky blue\n' >&8
wait_for "$B" 'inject: taken' 30 || echo "[mind-stale] note: inject not taken"
if wait_for "$B" 'remote teach arrived: "sky"->"blue"' 30; then res S0-hook PASS; else res S0-hook FAIL; fi

printf 'stop now\n' >&8
wait_for "$B" '^\[stop\] stopped 1 local 1' 30 || echo "[mind-stale] note: B not stopped"
printf 'mind teach fire warm\n' >&7
wait_for "$A" 'published mind/teach k=[0-9]+ v=[0-9]+ seq=1 ' 60 || echo "[mind-stale] note: A fire not published"
printf 'mind teach sky green\n' >&7
wait_for "$A" 'published mind/teach k=[0-9]+ v=[0-9]+ seq=2 ' 60 || echo "[mind-stale] note: A sky not published"
printf 'mind teach sea deep\n' >&7
wait_for "$A" 'published mind/teach k=[0-9]+ v=[0-9]+ seq=3 ' 60 || echo "[mind-stale] note: A sea not published"
sleep 5                                   # re-drives of sea, refused at B
if grep -aqE 'remote teach from node [0-9]+ refused \(stopped' "$B" \
   && ! grep -aqE 'remote teach arrived: "(fire|sea)"|"sky": "blue"->"green"' "$B"; then
    res S1-missed PASS; else res S1-missed FAIL; fi
printf 'stop resume\n' >&8

wait_for "$B" 'remote teach arrived: "sea"->"deep"' 60
wait_for "$B" 'remote teach arrived: "fire"->"warm"' 60
if grep -aqE 'remote teach arrived: "fire"->"warm"' "$B"; then res S2-have-pull PASS; else res S2-have-pull FAIL; fi
if grep -aqE 'remote teach arrived: "sea"->"deep"' "$B"; then res S3-redrive PASS; else res S3-redrive FAIL; fi
if wait_for "$B" 'remote REVISE key [0-9]+ "sky": "blue"->"green"' 60; then
    res S4-sky-green PASS; else res S4-sky-green FAIL; fi
grep -aE 'inject:|refused|mind/have from|remote teach arrived|remote REVISE|conflict rule' "$B" | head -20 | sed 's/^/  B: /'
cp "$A" /tmp/mind_stale.A.log 2>/dev/null; cp "$B" /tmp/mind_stale.B.log 2>/dev/null
echo "[mind-stale] logs: $WORK (and /tmp/mind_stale.[AB].log)"
if [ -z "$REDS" ]; then echo "[mind-stale] ALL PASS"; exit 0; fi
echo "[mind-stale] RED:$REDS"; exit 1
