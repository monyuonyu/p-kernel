#!/bin/bash
# tests/host/run_pfs_origin_send.sh — gap-ledger PFS-ORIGIN-SELF-ON-SEND,
# test-holes H38: a block that crosses the net keeps its owner.
#
# Expectations fixed before the code (2026-11-21):
#   O1 relay + 2 hosted nodes. Node 1 puts 80 blocks at once (more than the
#      64-slot table, so the first ones leave node 1's table before node 2
#      WANTs them); node 2 fetches every one of them within 90 s
#   O2 on node 2, the <id>.org of each of the 80 blocks says node 1 (its net
#      number 0), not 0xFF (self). master: 16 of them say self (probe
#      pk-scratch/b5md/h38b.sh, 11-21: "self 16 foreign 64") = red
#   O3 node 2's "replicated block ... origin=n<k>" lines for the 80 ids all
#      say n0 (none n255)
# Falsifier (must turn O2/O3 red, the plain build must be ALL PASS):
#   -DPFSR_NC_SEND_SELF  send_block_to sends "self" as before the fix
# Output: "[pfs-origin] <build> <check> PASS|FAIL", then ALL PASS (exit 0) or RED.
# Env: PFS_ORIGIN_MODES="plain nc" (default both), PFS_ORIGIN_PORT (relay
# port, default 7493), PFS_ORIGIN_WORK=<dir> keeps the logs.
# Rebuilds boot/linux* with EXTRA_CFLAGS and cleans it afterwards.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    *)             BOOT="$ROOT/boot/linux_x86_64" ;;
esac
unset PKERNEL_RELAY PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN PKERNEL_AUTONET PKERNEL_B5_ROUND_S
PIDS=""
cleanup() {
    for p in $PIDS; do kill -9 "$p" 2>/dev/null; done
    exec 11>&- 12>&- 2>/dev/null
    wait 2>/dev/null
    make -C "$BOOT" clean >/dev/null 2>&1
}
if [ -n "${PFS_ORIGIN_WORK:-}" ]; then
    WORK="$PFS_ORIGIN_WORK"; mkdir -p "$WORK"
    trap cleanup EXIT
else
    WORK="$(mktemp -d /tmp/pfsorg.XXXXXX)"
    trap 'cleanup; rm -rf "$WORK"' EXIT
fi
PORT="${PFS_ORIGIN_PORT:-7493}"
N=80
OWN=0      # node 1's number on the net (PKERNEL_NODE_ID 1 -> drpc node 0)
export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef

make -C "$ROOT/relay" > "$WORK/relay-build.log" 2>&1 || { echo "[pfs-origin] relay build FAIL"; exit 1; }

suite() {  # <tag> <cflags>
    local tag="$1" cf="$2" reds="" W k i id want got self fetched org
    r() { echo "[pfs-origin] $tag $1 $2"; [ "$2" = FAIL ] && reds="$reds $1"; return 0; }
    make -C "$BOOT" clean >/dev/null 2>&1
    if ! make -C "$BOOT" EXTRA_CFLAGS="$cf" > "$WORK/$tag-build.log" 2>&1; then
        r build FAIL; tail -20 "$WORK/$tag-build.log"; SUITE_REDS="build"; return
    fi
    W="$WORK/$tag"; rm -rf "$W"; mkdir -p "$W"
    PIDS=""
    "$ROOT/relay/relay" -p "$PORT" > "$W/relay.log" 2>&1 &
    PIDS="$PIDS $!"
    sleep 1
    for k in 1 2; do
        mkfifo "$W/f$k"; mkdir -p "$W/d$k"
        env PKERNEL_NODE_ID=$k PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$W/d$k" \
            PKERNEL_RELAY="127.0.0.1:$PORT" PKERNEL_RELAY_AUTOFALLBACK=0 PKERNEL_DEVICE_TIER=S \
            "$BOOT/p-kernel" < "$W/f$k" > "$W/n$k.log" 2>&1 &
        PIDS="$PIDS $!"
        eval "exec $((10 + k))<>\"$W/f$k\""
    done
    say() { printf '%s\n' "$2" >&$((10 + $1)); }
    sleep 25
    : > "$W/ids.txt"
    for i in $(seq 1 $N); do
        say 1 "pfs put h38-origin-block-$i-from-node-1"
        printf '%s' "h38-origin-block-$i-from-node-1" | sha256sum | cut -c1-64 >> "$W/ids.txt"
    done
    # O1: wait (up to 90 s) until node 2 holds all N
    for i in $(seq 1 90); do
        fetched=0
        while read -r id; do [ -e "$W/d2/$id" ] && fetched=$((fetched + 1)); done < "$W/ids.txt"
        [ "$fetched" = "$N" ] && break
        sleep 1
    done
    sleep 2
    say 1 exit; say 2 exit; sleep 2
    for p in $PIDS; do kill -9 "$p" 2>/dev/null; done
    exec 11>&- 12>&-
    wait 2>/dev/null
    PIDS=""
    echo "[pfs-origin] $tag node 2 holds $fetched of $N"
    [ "$fetched" = "$N" ] && r O1 PASS || r O1 FAIL
    # O2: each fetched block's .org (2 bytes little endian) says node 1's net
    # number, OWN (PKERNEL_NODE_ID - 1 = 0; the first test-first run counted 1
    # and saw 0 on 64 blocks = the test's error, corrected before the fix ran)
    want=0; self=0; got=0
    while read -r id; do
        [ -e "$W/d2/$id" ] || continue
        org=$(od -An -tu2 -N2 "$W/d2/$id.org" 2>/dev/null | tr -d ' \n')
        if [ "$org" = "$OWN" ]; then want=$((want + 1))
        elif [ "$org" = 255 ]; then self=$((self + 1))
        else got=$((got + 1)); fi
    done < "$W/ids.txt"
    echo "[pfs-origin] $tag node 2 .org: node 1 ($OWN) $want, self $self, other/none $got"
    [ "$want" = "$fetched" ] && [ "$fetched" -gt 0 ] && r O2 PASS || r O2 FAIL
    # O3: the receive lines for these ids (the log prints the first 16 hex)
    want=0; self=0
    while read -r id; do
        org=$(grep -a "replicated block id=${id:0:16} " "$W/n2.log" | head -1 | tr -d '\r' | sed -n 's/.*origin=n\([0-9]*\).*/\1/p')
        if [ "$org" = "$OWN" ]; then want=$((want + 1)); elif [ "$org" = 255 ]; then self=$((self + 1)); fi
    done < "$W/ids.txt"
    echo "[pfs-origin] $tag node 2 receive lines: origin=n$OWN $want, origin=n255 $self"
    [ "$self" = 0 ] && [ "$want" = "$fetched" ] && [ "$fetched" -gt 0 ] && r O3 PASS || r O3 FAIL
    SUITE_REDS="$reds"
}

MODES="${PFS_ORIGIN_MODES:-plain nc}"
bad=0
for m in $MODES; do
    case "$m" in
        plain) suite plain ""
               [ -n "$SUITE_REDS" ] && bad=1 ;;
        nc)    suite nc "-DPFSR_NC_SEND_SELF"
               case "$SUITE_REDS" in *O2*) echo "[pfs-origin] nc turns O2 red (as meant)" ;;
                   *) echo "[pfs-origin] nc stayed green on O2 (the falsifier does not bite)"; bad=1 ;; esac ;;
        *) echo "[pfs-origin] unknown mode $m"; bad=1 ;;
    esac
done
if [ "$bad" = 0 ]; then echo "[pfs-origin] ALL PASS"; exit 0; fi
echo "[pfs-origin] RED"; exit 1
