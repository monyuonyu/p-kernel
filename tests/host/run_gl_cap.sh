#!/bin/bash
# tests/host/run_gl_cap.sh — [gl-cap] G4 of N3 step 3 (research/n3-gl-region.md §3):
# the peer-copy slots of gossip_learn are capped and the cap is kept live.
#
# relay + 4 hosted nodes built with -DGL_PC_SLOTS=2 (each node can hold 2 of
# its 3 peers' models), `dtr gossip run` on all four:
#   G4a every node finishes (a "RESULT rounds=" line) = nothing crashed
#   G4b no "[g22-live] ... peers=N" line on any node has N > 2 (the merge folds
#       at most the 2 slots)
#   G4c at least one "[gl] peer cache full: refused" line (the third peer was
#       refused, not silently dropped)
# Falsifier: GLCAP_CFLAGS="-DGL_PC_SLOTS=2 -DGL_NC_NO_CAP" (the cap not kept)
# => G4b and G4c red. Rebuilds boot/linux* and cleans it. Not in CI.
# Env: GLCAP_CFLAGS (default -DGL_PC_SLOTS=2), GLCAP_PORT, GLCAP_WORK (kept).
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    *)             BOOT="$ROOT/boot/linux_x86_64" ;;
esac
unset PKERNEL_RELAY PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN PKERNEL_AUTONET
PORT="${GLCAP_PORT:-7451}"
CFL="${GLCAP_CFLAGS:--DGL_PC_SLOTS=2}"
ROUNDS=8; LOCAL=4
PIDS=""
cleanup() {
    for p in $PIDS; do kill -9 "$p" 2>/dev/null; done
    for k in 1 2 3 4; do eval "exec $((10 + k))>&-" 2>/dev/null; done
    wait 2>/dev/null
    make -C "$BOOT" clean >/dev/null 2>&1
}
if [ -n "${GLCAP_WORK:-}" ]; then W="$GLCAP_WORK"; mkdir -p "$W"; trap cleanup EXIT
else W="$(mktemp -d /tmp/glcap.XXXXXX)"; trap 'cleanup; rm -rf "$W"' EXIT; fi
echo "[gl-cap] build EXTRA_CFLAGS=$CFL"
make -C "$BOOT" clean >/dev/null 2>&1
make -C "$BOOT" EXTRA_CFLAGS="$CFL" > "$W/build.log" 2>&1 || { echo "[gl-cap] build FAIL"; exit 1; }
make -C "$ROOT/relay" > "$W/relay-build.log" 2>&1 || { echo "[gl-cap] relay build FAIL"; exit 1; }
export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
export PKERNEL_REGION_TAU_MS=250
"$ROOT/relay/relay" -p "$PORT" > "$W/relay.log" 2>&1 &
PIDS="$PIDS $!"
sleep 1
for k in 1 2 3 4; do
    rm -rf "$W/d$k" "$W/f$k" "$W/n$k.log"; mkfifo "$W/f$k"; mkdir -p "$W/d$k"
    env PKERNEL_NODE_ID=$k PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$W/d$k" \
        PKERNEL_RELAY_HOST=127.0.0.1 PKERNEL_RELAY_PORT="$PORT" \
        "$BOOT/p-kernel" < "$W/f$k" > "$W/n$k.log" 2>&1 &
    PIDS="$PIDS $!"
    eval "exec $((10 + k))<>\"$W/f$k\""
done
say() { printf '%s\n' "$2" >&$((10 + $1)); }
wf() { local n=0; while [ $n -lt $(( $3 * 2 )) ]; do grep -aqE "$2" "$1" 2>/dev/null && return 0; sleep 0.5; n=$((n+1)); done; return 1; }
for k in 1 2 3 4; do wf "$W/n$k.log" -- '-> FULL' 90 || echo "[gl-cap] node $k: never FULL"; done
sleep 3
for k in 1 2 3 4; do say $k "dtr gossip run $ROUNDS $LOCAL"; done
bad=0; done_n=0
for k in 1 2 3 4; do wf "$W/n$k.log" 'RESULT rounds=' 400 && done_n=$((done_n + 1)); done
if [ "$done_n" = 4 ]; then echo "[gl-cap] G4a PASS (4 of 4 finished)"; else echo "[gl-cap] G4a FAIL ($done_n of 4 finished)"; bad=1; fi
maxp=$(cat "$W"/n?.log | tr -d '\r' | grep -a '\[g22-live\]' | grep -aoE 'peers=[0-9]+' | cut -d= -f2 | sort -n | tail -1)
over=$(cat "$W"/n?.log | tr -d '\r' | grep -a '\[g22-live\]' | grep -aoE 'peers=[0-9]+' | cut -d= -f2 | awk '$1 > 2' | wc -l)
if [ -n "$maxp" ] && [ "$over" = 0 ]; then echo "[gl-cap] G4b PASS (the most peers folded: $maxp)"; else echo "[gl-cap] G4b FAIL (most peers ${maxp:-none}, lines over 2: $over)"; bad=1; fi
ref=$(cat "$W"/n?.log | grep -ac '\[gl\] peer cache full: refused')
if [ "$ref" -ge 1 ]; then echo "[gl-cap] G4c PASS ($ref refused lines)"; else echo "[gl-cap] G4c FAIL (no refused line)"; bad=1; fi
if [ "$bad" = 0 ]; then echo "[gl-cap] ALL PASS"; exit 0; fi
echo "[gl-cap] RED"; exit 1
