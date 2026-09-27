#!/bin/bash
# ---------------------------------------------------------------------------
# [swim-env-rounds] — test-only SWIM patience (inbox #3-4, 2026-09-27).
#
# A multi-node cert whose nodes block for tens of seconds (a teacher's GGUF
# generation, a student's distillation — the [ct-live-teach] blocker found
# 2026-09-22, conversational-teaching.md CT-2) gets its peers marked DEAD by
# SWIM mid-test. The product timing stays as it is (SWIM_SUSPECT_ROUNDS=2,
# SWIM_DEAD_ROUNDS=3 in swim.h). A HOSTED build reads two env overrides:
#     PKERNEL_SWIM_SUSPECT_ROUNDS / PKERNEL_SWIM_DEAD_ROUNDS   (1..250)
# Bare metal has no env and compiles the constants unchanged (crown-neutral).
#
# Two nodes over a local relay. A blocking node is simulated by SIGSTOP on
# node 2 for STOP_SECS, then SIGCONT.
#   arm DEFAULT: no env -> node 1 must log node 2 DEAD AFTER the stop (a DEAD in
#                the startup discovery race does not count)
#   arm ENV    : both nodes with the env set to 40 -> node 1 must NOT log
#                "node 2 -> DEAD", and must log the override line (so a
#                build that ignores the env cannot pass by being slow)
# Both arms must first see node 2 discovered by node 1 (mesh formed), or the
# arm is FAIL, not PASS (an unformed mesh never kills anyone).
# SWIM numbers nodes from 0: PKERNEL_NODE_ID=2 is "node 1" in node 1's log.
#
# Usage: tests/host/run_swim_env_rounds.sh     exit 0 = both arms as expected
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "unsupported host arch"; exit 1 ;;
esac
[ -x "$BOOT/p-kernel" ]    || make -C "$BOOT"       >/dev/null || exit 1
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay" >/dev/null || exit 1

STOP_SECS="${SWIM_ENV_STOP_SECS:-15}"
PORT="${SWIM_ENV_PORT:-7431}"
LOGDIR="${SWIM_ENV_LOGDIR:-$(mktemp -d)}"
export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
export PKERNEL_RELAY_HOST=127.0.0.1
export PKERNEL_RELAY_PORT="$PORT"
export PKERNEL_GALAXY=0

PASS=0 FAIL=0
ok()  { echo "  PASS $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL $1"; FAIL=$((FAIL+1)); }

RPID="" N1="" N2=""
stop_all() {
    for p in $N1 $N2; do kill -CONT "$p" 2>/dev/null; kill "$p" 2>/dev/null; done
    [ -n "$RPID" ] && kill "$RPID" 2>/dev/null
    for p in $N1 $N2 $RPID; do wait "$p" 2>/dev/null; done
    exec 7>&- 8>&- 2>/dev/null
    N1="" N2="" RPID=""
}
trap stop_all EXIT

# run_arm <name> <env-rounds or empty>
run_arm() {
    local name="$1" rounds="$2" d="$LOGDIR/$1"
    mkdir -p "$d"; rm -f "$d"/*
    local envs=()
    [ -n "$rounds" ] && envs=(PKERNEL_SWIM_SUSPECT_ROUNDS="$rounds" PKERNEL_SWIM_DEAD_ROUNDS="$rounds")

    "$ROOT/relay/relay" -p "$PORT" >"$d/relay.log" 2>&1 & RPID=$!
    sleep 1
    mkfifo "$d/in1" "$d/in2"
    env "${envs[@]}" PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 "$BOOT/p-kernel" <"$d/in1" >"$d/node1.log" 2>&1 & N1=$!
    exec 7>"$d/in1"
    env "${envs[@]}" PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 "$BOOT/p-kernel" <"$d/in2" >"$d/node2.log" 2>&1 & N2=$!
    exec 8>"$d/in2"

    local t=0
    while ! grep -aq "node 1 discovered\|node 1 -> ALIVE\|node 1 recovered" "$d/node1.log" && [ "$t" -lt 30 ]; do sleep 1; t=$((t+1)); done
    sleep "${SWIM_ENV_SETTLE:-10}"   # let a startup SUSPECT/DEAD race settle back to ALIVE
    echo "[$name] mesh after ${t}s; SIGSTOP node 2 for ${STOP_SECS}s"
    wc -l < "$d/node1.log" > "$d/stopline"   # DEAD is judged only after this line
    kill -STOP "$N2"
    sleep "$STOP_SECS"
    kill -CONT "$N2"
    sleep 3
    printf 'exit\n' >&7; printf 'exit\n' >&8
    sleep 1
    stop_all
}

echo "[swim-env-rounds] build: $BOOT/p-kernel  stop=${STOP_SECS}s  logs=$LOGDIR"

run_arm default ""
L="$LOGDIR/default/node1.log"
grep -aq "node 1 discovered\|node 1 -> ALIVE\|node 1 recovered" "$L" && ok "default: mesh formed" || bad "default: node 1 never saw node 2"
grep -aq "\[swim\] rounds suspect=" "$L" && bad "default: override line printed without env" || ok "default: no override line"
tail -n +"$(( $(cat "$LOGDIR/default/stopline") + 1 ))" "$L" | grep -aq "node 1 -> DEAD" \
    && ok "default: blocked node 2 declared DEAD after the stop (product timing)" \
    || bad "default: node 2 not declared DEAD in ${STOP_SECS}s"

run_arm env 40
L="$LOGDIR/env/node1.log"
grep -aq "node 1 discovered\|node 1 -> ALIVE\|node 1 recovered" "$L" && ok "env: mesh formed" || bad "env: node 1 never saw node 2"
grep -aq "\[swim\] rounds suspect=40 dead=40 (env)" "$L" && ok "env: override read" || bad "env: override line missing"
grep -aq "node 1 -> DEAD" "$L" && bad "env: node 2 declared DEAD despite PKERNEL_SWIM_*_ROUNDS=40" \
    || ok "env: blocked node 2 survived ${STOP_SECS}s"

echo "[swim-env-rounds] RESULT: $PASS PASS / $FAIL FAIL"
[ "$FAIL" = 0 ] && { echo "[swim-env-rounds] ALL PASS"; exit 0; }
exit 1
