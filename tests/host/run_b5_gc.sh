#!/bin/bash
# tests/host/run_b5_gc.sh — B5 ① (research/b5-m-default.md §3 2, §4 D-G1): the
# resident round's student chunks are cleaned up, so the store stops growing.
#
# TEST FIRST (2026-11-21, before ① is built; master is red, measured X1 there):
# relay + 5 hosted nodes, tier S, PKERNEL_B5_ROUND_S=120, 4 rounds (the X1 rig
# of pk-scratch/b5md/x1.sh). The store of each node (du -sb of its
# PKERNEL_PFS_DIR) at every round head; heads are numbered from the first head
# after the start (G1) as 1.
#   G0 (precondition) at least 15 of the 20 merges say "members 5 fetched 4"
#      (the rounds really ran)
#   G1 on every node, the store at head K+2 <= 1.1 x the store at head K
#      (D-G1). master 11-21: 11.41 MB -> 30.26 MB (2.65x), feat/b5-zmv
#      5.17 -> 11.52 MB (2.23x) = red
#      K: CHANGED AFTER SEEING RESULTS (11-21 08:50 run, after audit-203 🔁B5GC).
#      It was fixed at 2. Audit-203 found G1 depends on the start phase: plain
#      555a031d went 1.97 -> 5.17 -> 5.85 -> 5.85 -> 5.86 MB (flat from head 3)
#      but head 2 was before the first cleanup, so 1.13 = red; the implementer's
#      47e4c2b4 run 2 (1.62) had the same shape and fd4739b6's 2/2 was a good
#      phase. Now K = the first head (>= 2) at which every node has printed an
#      "own generation two back" cleanup line (the store can only be flat after
#      that); if that never happens (e.g. -DB5_NC_NO_GC), K = 2 as before, so a
#      build without cleanup is still judged by its growth. 8 heads (NR=7), so
#      K+2 exists for K up to 6; else G1 FAIL (not enough heads).
#   G2 no line "not all here" / "cannot be read" / "gave up" in any node log
#      (added the same day, test-holes: audit-203 b1 = the guard holds only this
#      node's refs dropped too much: G1 could not tell it from plain, these lines
#      were 43 for b1, 58 for b3 (no guard), 0 for plain, NO_GC and the N2C3 runs)
# Falsifier: whatever ① adds to turn it off (e.g. -DB5_NC_NO_GC) must turn G1
# red; give it as B5GC_NC_CFLAGS, the plain build must be ALL PASS. Not in CI
# until ① is built (it is red now, ~11 minutes).
# Output: "[b5-gc] <check> PASS|FAIL ...", then ALL PASS (exit 0) or RED.
# Env: B5GC_PORT (relay, default 7531), B5GC_WORK=<dir> keeps the logs,
# B5GC_CFLAGS (EXTRA_CFLAGS of the build). Rebuilds boot/linux* and cleans it.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    *)             BOOT="$ROOT/boot/linux_x86_64" ;;
esac
unset PKERNEL_RELAY PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN PKERNEL_AUTONET
PORT="${B5GC_PORT:-7531}"; P=120; NR=7
PIDS=""
cleanup() {
    for p in $PIDS; do kill -9 "$p" 2>/dev/null; done
    for k in 1 2 3 4 5; do eval "exec $((10 + k))>&-" 2>/dev/null; done
    wait 2>/dev/null
    make -C "$BOOT" clean >/dev/null 2>&1
}
if [ -n "${B5GC_WORK:-}" ]; then W="$B5GC_WORK"; mkdir -p "$W"; trap cleanup EXIT
else W="$(mktemp -d /tmp/b5gc.XXXXXX)"; trap 'cleanup; rm -rf "$W"' EXIT; fi
make -C "$BOOT" clean >/dev/null 2>&1
make -C "$BOOT" EXTRA_CFLAGS="${B5GC_CFLAGS:-}" > "$W/build.log" 2>&1 || { echo "[b5-gc] build FAIL"; exit 1; }
make -C "$ROOT/relay" > "$W/relay-build.log" 2>&1 || { echo "[b5-gc] relay build FAIL"; exit 1; }
export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
"$ROOT/relay/relay" -p "$PORT" > "$W/relay.log" 2>&1 &
PIDS="$PIDS $!"
sleep 1
for k in 1 2 3 4 5; do
    rm -rf "$W/d$k" "$W/f$k"; mkfifo "$W/f$k"; mkdir -p "$W/d$k"
    env PKERNEL_NODE_ID=$k PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$W/d$k" PKERNEL_RELAY="127.0.0.1:$PORT" \
        PKERNEL_DEVICE_TIER=S PKERNEL_B5_ROUND_S="$P" \
        "$BOOT/p-kernel" < "$W/f$k" > "$W/n$k.log" 2>&1 &
    PIDS="$PIDS $!"
    eval "exec $((10 + k))<>\"$W/f$k\""
done
say() { printf '%s\n' "$2" >&$((10 + $1)); }
wf() { local n=0; while [ $n -lt $(( $3 * 2 )) ]; do grep -aqE "$2" "$1" 2>/dev/null && return 0; sleep 0.5; n=$((n+1)); done; return 1; }
for k in 1 2 3 4 5; do wf "$W/n$k.log" 'student_region_task up' 180 || echo "[b5-gc] node $k: no task"; done
for k in 1 2 3 4 5; do say $k "baby 1 0.00$k 32"; done
for k in 1 2 3 4 5; do wf "$W/n$k.log" 'held-out loss \(after\)' 300 || echo "[b5-gc] node $k: no trained line"; done
now=$(date +%s); G1=$(( (now + 20 + P - 1) / P ))
: > "$W/heads.txt"; : > "$W/own.txt"
for r in $(seq 0 "$NR"); do
    h=$(( (G1 + r) * P )); t=$(( h - 1 - $(date +%s) )); [ $t -gt 0 ] && sleep $t
    line="$((r + 1))"
    for k in 1 2 3 4 5; do line="$line $(du -sb "$W/d$k" | cut -f1)"; done
    own=0
    for k in 1 2 3 4 5; do grep -aq 'own generation two back' "$W/n$k.log" && own=$((own + 1)); done
    echo "$line" >> "$W/heads.txt"
    echo "$own" >> "$W/own.txt"
    echo "[b5-gc] head $line (nodes with an own cleanup: $own)"
done
for k in 1 2 3 4 5; do say $k exit; done
sleep 2
merges=$(cat "$W"/n?.log | grep -a '\[baby\] auto: gen' | grep -c 'members 5 fetched 4')
echo "[b5-gc] merges with members 5 fetched 4: $merges of 20"
bad=0
if [ "$merges" -ge 15 ]; then echo "[b5-gc] G0 PASS"; else echo "[b5-gc] G0 FAIL (the rounds did not run)"; bad=1; fi
K=$(awk 'NR >= 2 && $1 == 5 {print NR; exit}' "$W/own.txt")
if [ -z "$K" ]; then K=2; echo "[b5-gc] no head with an own cleanup on all 5 nodes: K = 2"
else echo "[b5-gc] first head with an own cleanup on all 5 nodes: K = $K"; fi
g1=PASS
if [ $((K + 2)) -gt $((NR + 1)) ]; then
    echo "[b5-gc] G1 FAIL (not enough heads: K $K, last head $((NR + 1)))"; g1=FAIL
else
    hk=$(sed -n "${K}p" "$W/heads.txt"); hk2=$(sed -n "$((K + 2))p" "$W/heads.txt")
    for k in 1 2 3 4 5; do
        a=$(echo "$hk" | cut -d' ' -f$((k + 1))); b=$(echo "$hk2" | cut -d' ' -f$((k + 1)))
        r=$(python3 -c "import sys; a,b=int(sys.argv[1]),int(sys.argv[2]); print('%.2f' % (b / a if a else 99))" "$a" "$b")
        ok=$(python3 -c "import sys; a,b=int(sys.argv[1]),int(sys.argv[2]); print(1 if a and b <= 1.1 * a else 0)" "$a" "$b")
        echo "[b5-gc] node $k: head $K $a B, head $((K + 2)) $b B, ratio $r"
        [ "$ok" = 1 ] || g1=FAIL
    done
fi
echo "[b5-gc] G1 $g1"
[ "$g1" = PASS ] || bad=1
over=$(cat "$W"/n?.log | grep -acE 'not all here|cannot be read|student fetch: gave up')
if [ "$over" -eq 0 ]; then echo "[b5-gc] G2 PASS (no too-much-dropped line)"
else echo "[b5-gc] G2 FAIL ($over lines: not all here / cannot be read / gave up)"; bad=1; fi
if [ "$bad" = 0 ]; then echo "[b5-gc] ALL PASS"; exit 0; fi
echo "[b5-gc] RED"; exit 1
