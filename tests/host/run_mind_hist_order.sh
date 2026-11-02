#!/bin/bash
# tests/host/run_mind_hist_order.sh — [mind-hist-order] (ROADMAP 1-3m).
#
# mhi_tick reads the mind/hist sample before it publishes: the topic is one
# shared LATEST_ONLY slot, and a publish first overwrites what a peer sent
# (634a7da4, the same fix as mh_tick). [mind-stale] with -DMH_EVERY=1 tells the
# two orders apart for mind/have only; nothing did for mind/hist.
#
# Two hosted nodes built with -DMT_RX_INJECT -DMHI_EVERY=1 (both send on every
# tick, so the bad phase is the only phase):
#   A (id 1) gets sky green, B (id 2) gets fire warm (stamp 3, nodes that are
#   not there); mind/have and the pull give each the other's fact
#   A gets a losing sky blue, B a losing fire blue: each refuses it and keeps
#   a record of its own, and from then on both send on every tick
#   H1 B reads A's sky record as history, H2 A reads B's fire record
# Expected before the first run: green here, H1 and H2 red with
# -DMH_PUB_FIRST (the old order) and with -DMH_HIST_IGNORE.
# Output "[mind-hist-order] <check> PASS|FAIL", then ALL PASS (exit 0) or RED
# (exit 1). Env: MHO_PORT (default 7495), EXTRA_CFLAGS (added to the two
# flags). Rebuilds boot/linux* and cleans it afterwards.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    *)             BOOT="$ROOT/boot/linux_x86_64" ;;
esac
PORT="${MHO_PORT:-7495}"
WORK="$(mktemp -d /tmp/mho.XXXXXX)"
REDS=""; RELAY=0; PA=0; PB=0
res() { echo "[mind-hist-order] $1 $2"; [ "$2" = FAIL ] && REDS="$REDS $1"; return 0; }
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
make -C "$BOOT" EXTRA_CFLAGS="-DMT_RX_INJECT -DMHI_EVERY=1 ${EXTRA_CFLAGS:-}" > "$WORK/build.log" 2>&1 \
    || { echo "[mind-hist-order] BUILD FAILED"; tail -20 "$WORK/build.log"; exit 1; }

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
wait_for "$A" 'mind_net_task up' 90 || echo "[mind-hist-order] note: A mind_net_task not seen"
wait_for "$B" 'mind_net_task up' 90 || echo "[mind-hist-order] note: B mind_net_task not seen"

printf 'mind inject 5 3 sky green\n' >&7
printf 'mind inject 6 3 fire warm\n' >&8
# 90 s: the first mind use of a node runs the substrate pretrain (run_mind_stale.sh)
wait_for "$A" 'remote teach arrived: "sky"->"green"' 90 || echo "[mind-hist-order] note: A has no sky"
wait_for "$B" 'remote teach arrived: "fire"->"warm"' 90 || echo "[mind-hist-order] note: B has no fire"
# each learns the other's fact by mind/have -> want -> pull
if wait_for "$A" 'remote teach arrived: "fire"->"warm"' 60 \
   && wait_for "$B" 'remote teach arrived: "sky"->"green"' 60; then
    res H0-both-hold-both PASS; else res H0-both-hold-both FAIL; fi

printf 'mind inject 5 1 sky blue\n' >&7
wait_for "$A" 'inject: taken' 30 || echo "[mind-hist-order] note: A inject not taken"
printf 'mind inject 6 1 fire blue\n' >&8
wait_for "$B" 'inject: taken' 30 || echo "[mind-hist-order] note: B inject not taken"
SKY='conflict recorded: key [0-9]+ "sky": "blue"'
FIRE='conflict recorded: key [0-9]+ "fire": "blue"'
if wait_for "$A" "$SKY.*refused on arrival" 30 && wait_for "$B" "$FIRE.*refused on arrival" 30; then
    res H0-records PASS; else res H0-records FAIL; fi

sleep 40                                  # both send on every tick from here on
printf 'mind conflicts\n' >&7; printf 'mind conflicts\n' >&8
sleep 3
if grep -aqE '"sky": "blue".*heard as history from node 0' "$B"; then
    res H1-B-heard-A PASS; else res H1-B-heard-A FAIL; fi
if grep -aqE '"fire": "blue".*heard as history from node 1' "$A"; then
    res H2-A-heard-B PASS; else res H2-A-heard-B FAIL; fi
grep -aE 'inject:|conflict recorded|heard as history|mind/hist' "$A" | head -8 | sed 's/^/  A: /'
grep -aE 'inject:|conflict recorded|heard as history|mind/hist' "$B" | head -8 | sed 's/^/  B: /'
cp "$A" /tmp/mind_hist_order.A.log 2>/dev/null; cp "$B" /tmp/mind_hist_order.B.log 2>/dev/null
echo "[mind-hist-order] logs: $WORK (and /tmp/mind_hist_order.[AB].log)"
if [ -z "$REDS" ]; then echo "[mind-hist-order] ALL PASS"; exit 0; fi
echo "[mind-hist-order] RED:$REDS"; exit 1
