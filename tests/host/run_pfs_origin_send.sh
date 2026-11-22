#!/bin/bash
# tests/host/run_pfs_origin_send.sh — gap-ledger PFS-ORIGIN-SELF-ON-SEND,
# test-holes H38: a block that crosses the net keeps its owner.
#
# Expectations fixed before the code (2026-11-21):
#   O1 relay + 2 hosted nodes. Node 1 puts 80 blocks at once (more than the
#      64-slot table, so the first ones leave node 1's table before node 2
#      WANTs them); node 2 fetches every one of them within 90 s
#      -> changed 11-21 02:5x, after the fix's 4th run gave 73 of 80 under a
#      busy container: #49-55 were announced by node 1 (fanout 1) and never
#      heard by node 2 = the 0 ms burst's announce loss, a known limit
#      (gap-ledger PFS-REPL-WANT-DROP), not the origin. Master's 16 "self"
#      were #1-16 in all 3 runs (the ones out of node 1's table), so now:
#      node 2 holds ALL of #1-16 and at least 64 of the 80; O2/O3 judge every
#      block it holds
#   O2 on node 2, the <id>.org of each of the 80 blocks says node 1 (its net
#      number 0), not 0xFF (self). master: 16 of them say self (probe
#      pk-scratch/b5md/h38b.sh, 11-21: "self 16 foreign 64") = red
#   O3 node 2's "replicated block ... origin=n<k>" lines for the 80 ids all
#      say n0 (none n255)
# Added after the first fix run (02:37), before its code: O1-O3 pass on the
# self->number line alone (node 1's own blocks); they do not see the <id>.org
# read (a peer's block out of the table). So:
#   O4 `pfs orgtest` (one node, PKERNEL_PFS_DIR): a peer's block (origin 7)
#      and an own block pushed out of the table, two more in it; the origin
#      each is sent with is 7 / this node / 7 / this node ("[pfs-orgtest] PASS")
# Added 2026-11-22 (CI 44994993940: nc did not bite, n255 0): the 80 puts go
# in one write, and
#   O5 node 1 sent each of the first 16 after its last put (= out of its
#      table when sent), 16 of 16; on the nc build too (else not exercised)
# Falsifiers (the plain build must be ALL PASS):
#   -DPFSR_NC_SEND_SELF   "self" goes on the wire as before: O2, O3, O4 red
#   -DPFSR_NC_NO_ORG_READ the <id>.org is not read: O4 red (nc2 runs O4 only)
# Output: "[pfs-origin] <build> <check> PASS|FAIL", then ALL PASS (exit 0) or RED.
# Env: PFS_ORIGIN_MODES="plain nc nc2" (default all), PFS_ORIGIN_PORT (relay
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

suite() {  # <tag> <cflags> [o4only]
    local tag="$1" cf="$2" only="${3:-}" reds="" W k i id want got self fetched first org
    r() { echo "[pfs-origin] $tag $1 $2"; [ "$2" = FAIL ] && reds="$reds $1"; return 0; }
    make -C "$BOOT" clean >/dev/null 2>&1
    if ! make -C "$BOOT" EXTRA_CFLAGS="$cf" > "$WORK/$tag-build.log" 2>&1; then
        r build FAIL; tail -20 "$WORK/$tag-build.log"; SUITE_REDS="build"; return
    fi
    W="$WORK/$tag"; rm -rf "$W"; mkdir -p "$W"
    # O4: one node off the net
    mkdir -p "$W/d0"
    printf 'pfs orgtest\nexit\n' | timeout 120 env PKERNEL_NODE_ID=1 PKERNEL_PFS_DIR="$W/d0" \
        "$BOOT/p-kernel" > "$W/o4.log" 2>&1
    grep -a '^\[pfs-orgtest\]' "$W/o4.log" | tr -d '\r' | sed "s/^/[pfs-origin] $tag /"
    grep -aq '^\[pfs-orgtest\] PASS' "$W/o4.log" && r O4 PASS || r O4 FAIL
    if [ "$only" = o4only ]; then SUITE_REDS="$reds"; return; fi
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
    # The ids first, then the 80 puts in one write (2026-11-22, CI 44994993940):
    # with three forks per put between the puts, a slow runner's shell let node
    # 1 send #1 after about 42 puts = still in its table, so the "out of the
    # table" premise of O1-O3 did not hold and the nc falsifier did not bite
    # (n255 16 in the container, 6 and 0 on CI). O5 below checks the premise.
    : > "$W/ids.txt"; : > "$W/puts.txt"
    for i in $(seq 1 $N); do
        printf 'pfs put h38-origin-block-%s-from-node-1\n' "$i" >> "$W/puts.txt"
        printf '%s' "h38-origin-block-$i-from-node-1" | sha256sum | cut -c1-64 >> "$W/ids.txt"
    done
    cat "$W/puts.txt" >&11
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
    first=0
    head -16 "$W/ids.txt" > "$W/first16.txt"
    while read -r id; do [ -e "$W/d2/$id" ] && first=$((first + 1)); done < "$W/first16.txt"
    echo "[pfs-origin] $tag node 2 holds $fetched of $N, $first of the first 16 (out of node 1's table)"
    [ "$first" = 16 ] && [ "$fetched" -ge 64 ] && r O1 PASS || r O1 FAIL
    # On an O1 red, say where the blocks stopped (CI 44961258707 held 8 of 80
    # and kept no logs). Prints only; the verdict above does not change.
    case "$reds" in *O1*)
        echo "[pfs-origin] $tag diag O1: n1 announces $(grep -ac '\[pfs\] announce id=' "$W/n1.log")," \
             "fanout=0 $(grep -ac 'announce id=.*fanout=0' "$W/n1.log"), queue-full $(grep -ac 'announce queue full' "$W/n1.log");" \
             "n2 heard $(grep -ac 'heard announce' "$W/n2.log"), replicated $(grep -ac 'replicated block' "$W/n2.log")"
        grep -aiE 'dropped|SUSPECT|DEAD|level change|relay (lost|down|reconnect)' "$W/n1.log" "$W/n2.log" \
            | tr -d '\r' | head -12 | sed "s|^$W/|[pfs-origin] $tag diag |"
        tail -3 "$W/relay.log" | tr -d '\r' | sed "s/^/[pfs-origin] $tag diag relay: /"
        echo "[pfs-origin] $tag diag after the kill: p-kernel processes $(ps -eo args | grep -c '[p]-kernel$'), relays $(ps -eo args | grep -c '[r]elay/relay')," \
             "load $(cut -d' ' -f1-3 /proc/loadavg)" ;;
    esac
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
    # O5 (added 2026-11-22 before its first run): the premise of O1-O3. Each of
    # the first 16 was first sent by node 1 after its last put (so out of its
    # 64-slot table when sent): 16 of 16. Read from n1.log line order.
    lastput=$(grep -an '\[pfs\] put len=' "$W/n1.log" | tail -1 | cut -d: -f1)
    late=0
    while read -r id; do
        s=$(grep -an "sent block id=${id:0:16} " "$W/n1.log" | head -1 | cut -d: -f1)
        [ -n "$s" ] && [ -n "$lastput" ] && [ "$s" -gt "$lastput" ] && late=$((late + 1))
    done < "$W/first16.txt"
    echo "[pfs-origin] $tag node 1 sent $late of the first 16 after its last put (line ${lastput:-none})"
    [ "$late" = 16 ] && r O5 PASS || r O5 FAIL
    SUITE_REDS="$reds"
}

MODES="${PFS_ORIGIN_MODES:-plain nc nc2}"
bad=0
for m in $MODES; do
    case "$m" in
        plain) suite plain ""
               [ -n "$SUITE_REDS" ] && bad=1 ;;
        nc)    suite nc "-DPFSR_NC_SEND_SELF"
               case "$SUITE_REDS" in *O5*) echo "[pfs-origin] nc: the premise O5 did not hold, the falsifier was not exercised"; bad=1 ;; esac
               case "$SUITE_REDS" in *O2*) echo "[pfs-origin] nc turns O2 red (as meant)" ;;
                   *) echo "[pfs-origin] nc stayed green on O2 (the falsifier does not bite)"; bad=1 ;; esac
               case "$SUITE_REDS" in *O4*) ;; *) echo "[pfs-origin] nc stayed green on O4"; bad=1 ;; esac ;;
        nc2)   suite nc2 "-DPFSR_NC_NO_ORG_READ" o4only
               case "$SUITE_REDS" in *O4*) echo "[pfs-origin] nc2 turns O4 red (as meant)" ;;
                   *) echo "[pfs-origin] nc2 stayed green on O4 (the falsifier does not bite)"; bad=1 ;; esac ;;
        *) echo "[pfs-origin] unknown mode $m"; bad=1 ;;
    esac
done
if [ "$bad" = 0 ]; then echo "[pfs-origin] ALL PASS"; exit 0; fi
echo "[pfs-origin] RED"; exit 1
