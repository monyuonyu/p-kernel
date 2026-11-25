#!/bin/bash
# tests/host/run_b5_gate.sh — A1 of research/b5-android-gate.md: the B5 round
# gate (pk_b5_allow, r3_incontext.c) holds the rounds while closed.
#
# relay + 2 hosted nodes, tier S, PKERNEL_B5_ROUND_S=60. `baby gate 0` is sent
# at once (before student_region_task is up, as the JNI closes it at boot);
# each node trains its student (`baby 1 ...`), then:
#   A1a while closed for 130 s (two rounds' heads): no "auto: publish" line on
#       either node, and a "[baby] round gate closed" line on both
#   A1b after `baby gate 1`: an "auto: publish" line on both within 150 s, and
#       a "[baby] round gate open" line
# Falsifier: B5G_CFLAGS=-DB5_NC_NO_GATE (the task never reads the gate) => A1a red.
# Env: B5G_PORT (relay, default 7541), B5G_WORK=<dir> keeps the logs, B5G_CFLAGS.
# Rebuilds boot/linux* and cleans it. In CI: shipped-llm-certs (the plain arm
# ALL PASS, the NO_GATE arm red on A1a; its A1b is red too, as a task that
# never reads the gate never says "round gate open").
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    *)             BOOT="$ROOT/boot/linux_x86_64" ;;
esac
unset PKERNEL_RELAY PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN PKERNEL_AUTONET
PORT="${B5G_PORT:-7541}"; P=60
PIDS=""
cleanup() {
    for p in $PIDS; do kill -9 "$p" 2>/dev/null; done
    for k in 1 2; do eval "exec $((10 + k))>&-" 2>/dev/null; done
    wait 2>/dev/null
    make -C "$BOOT" clean >/dev/null 2>&1
}
if [ -n "${B5G_WORK:-}" ]; then W="$B5G_WORK"; mkdir -p "$W"; trap cleanup EXIT
else W="$(mktemp -d /tmp/b5g.XXXXXX)"; trap 'cleanup; rm -rf "$W"' EXIT; fi
make -C "$BOOT" clean >/dev/null 2>&1
make -C "$BOOT" EXTRA_CFLAGS="${B5G_CFLAGS:-}" > "$W/build.log" 2>&1 || { echo "[b5-gate] build FAIL"; exit 1; }
make -C "$ROOT/relay" > "$W/relay-build.log" 2>&1 || { echo "[b5-gate] relay build FAIL"; exit 1; }
export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
"$ROOT/relay/relay" -p "$PORT" > "$W/relay.log" 2>&1 &
PIDS="$PIDS $!"
sleep 1
for k in 1 2; do
    rm -rf "$W/d$k" "$W/f$k" "$W/n$k.log"; mkfifo "$W/f$k"; mkdir -p "$W/d$k"
    env PKERNEL_NODE_ID=$k PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$W/d$k" PKERNEL_RELAY="127.0.0.1:$PORT" \
        PKERNEL_DEVICE_TIER=S PKERNEL_B5_ROUND_S="$P" \
        "$BOOT/p-kernel" < "$W/f$k" > "$W/n$k.log" 2>&1 &
    PIDS="$PIDS $!"
    eval "exec $((10 + k))<>\"$W/f$k\""
done
say() { printf '%s\n' "$2" >&$((10 + $1)); }
wf() { local n=0; while [ $n -lt $(( $3 * 2 )) ]; do grep -aqE -- "$2" "$1" 2>/dev/null && return 0; sleep 0.5; n=$((n+1)); done; return 1; }
for k in 1 2; do say $k "baby gate 0"; done
for k in 1 2; do wf "$W/n$k.log" 'baby\] gate: closed' 60 || echo "[b5-gate] node $k: no gate reply"; done
for k in 1 2; do wf "$W/n$k.log" 'student_region_task up' 120 || echo "[b5-gate] node $k: no task"; done
for k in 1 2; do say $k "baby 1 0.00$k 32"; done
for k in 1 2; do wf "$W/n$k.log" 'held-out loss \(after\)' 300 || echo "[b5-gate] node $k: no trained line"; done
sleep 130
bad=0
pub=$(cat "$W"/n1.log "$W"/n2.log | grep -ac 'auto: publish')
shut=0; for k in 1 2; do grep -aq 'round gate closed' "$W/n$k.log" && shut=$((shut + 1)); done
if [ "$pub" = 0 ] && [ "$shut" = 2 ]; then echo "[b5-gate] A1a PASS (closed 130 s: 0 auto publish lines, 2/2 nodes said closed)"
else echo "[b5-gate] A1a FAIL (closed: $pub auto publish lines, $shut/2 nodes said closed)"; bad=1; fi
for k in 1 2; do say $k "baby gate 1"; done
ok=0; for k in 1 2; do wf "$W/n$k.log" 'auto: publish' 150 && grep -aq 'round gate open' "$W/n$k.log" && ok=$((ok + 1)); done
if [ "$ok" = 2 ]; then echo "[b5-gate] A1b PASS (opened: 2/2 nodes published)"
else echo "[b5-gate] A1b FAIL (opened: $ok/2 nodes published and said open)"; bad=1; fi
if [ "$bad" = 0 ]; then echo "[b5-gate] ALL PASS"; exit 0; fi
echo "[b5-gate] RED"; exit 1
