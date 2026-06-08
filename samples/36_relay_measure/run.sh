#!/bin/bash
# =============================================================================
# run.sh — REAL relay round-trip latency under load + a real end-to-end energy
#          proxy (survival-network.md §4; 俯瞰監査 G31/G25).
# -----------------------------------------------------------------------------
# §4 は MoE のスパース性を「光速とエネルギーの物理制約への答え」とする。
#   locality.md (wave-12): traffic/energy-proxy を kdds カウンタで実測。ただし
#                          (D) で「per-packet latency は未測」と正直に残した。
#   latency.md  (wave-15): MODELLED な far-delay を注入し二層分離を実測。だが
#                          relay 自身の転送 RTT も、負荷下の RTT も測っていない。
# 本波はその二点を REAL ./relay 上で実測する:
#   1) relay の転送 RTT を OFFERED LOAD(burst depth)の関数として(実測)。
#      relay の probe-stamp を ON にすると RTT を network vs relay-residence に分解。
#   2) per-message ENERGY proxy を END-TO-END で(実バイト = 2 hops 分を実測、
#      joule 換算と far の重み K は MODELLED と明示)。
#
# 何がモデルで何が実測か(正直に):
#   実測 : リアルな UDP socket 往復 RTT / 実バイト数(送受の両 leg)。
#   モデル: joule への換算係数(~1uJ/B 無線)・far リンクの重み K=5・距離。
#
# 非破壊の証明:
#   relay の wire 変更(probe-stamp)は OFF が既定。stampcheck を flag OFF/ON で
#   2 回走らせ、echo_extra=0(verbatim)→16(additive)を示す。さらに relay 6/6 を
#   flag OFF で回して緑のままを確認する。
#
# Usage: ./run.sh   考察は docs/benchmarks/survival.md
# =============================================================================
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
RELAY_DIR="$ROOT/relay"

PORT="${PORT:-27460}"
ENERGY_M="${ENERGY_M:-2000}"
ENERGY_P="${ENERGY_P:-256}"

export PKERNEL_RELAY_KEY="${PKERNEL_RELAY_KEY:-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef}"

# Build the real relay + the measurement client.
make -C "$RELAY_DIR" relay >/dev/null 2>&1 || { echo "relay build failed"; exit 1; }
cc -Wall -Wextra -O2 -std=gnu11 -I"$RELAY_DIR" \
    -o "$HERE/measure" "$HERE/measure.c" "$RELAY_DIR/sha256.c" \
    || { echo "client build failed"; exit 1; }

RELAY_PID=""
start_relay() {  # $1 = extra env assignment(s)
    LOG="$(mktemp /tmp/measure_relay.XXXXXX)"
    env $1 "$RELAY_DIR/relay" -p "$PORT" >"$LOG" 2>&1 &
    RELAY_PID=$!
    sleep 1
}
stop_relay() {
    [ -n "$RELAY_PID" ] && kill "$RELAY_PID" 2>/dev/null
    wait "$RELAY_PID" 2>/dev/null
    RELAY_PID=""
    rm -f "$LOG"
}
cleanup() { [ -n "$RELAY_PID" ] && kill "$RELAY_PID" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

fails=0

echo "=== 36_relay_measure: REAL relay RTT under load + end-to-end energy proxy ==="
echo "    port=$PORT  energy M=$ENERGY_M P=$ENERGY_P"
echo

# --- 0. PROVE the wire change is additive + off-by-default -------------------
echo "--- 0. wire change is additive + OFF by default ---"
start_relay ""                                   # probe-stamp NOT set
OFF="$("$HERE/measure" stampcheck "$PORT" | sed -n 's/.*echo_extra=\([0-9-]*\).*/\1/p')"
stop_relay
start_relay "PKERNEL_RELAY_PROBE_STAMP=1"        # probe-stamp ON
ON="$("$HERE/measure" stampcheck "$PORT" | sed -n 's/.*echo_extra=\([0-9-]*\).*/\1/p')"
stop_relay
echo "    flag OFF -> echo_extra=${OFF} B   (expect 0 = verbatim, relay-HA safe)"
echo "    flag ON  -> echo_extra=${ON} B  (expect 16 = rx_us+tx_us appended)"
if [ "${OFF:-x}" = "0" ] && [ "${ON:-x}" = "16" ]; then
    echo "    additive/off-by-default: PASS"
else
    echo "    additive/off-by-default: FAIL"; fails=$((fails+1))
fi
echo

# --- 1. relay forwarding RTT vs offered load (probe-stamp ON for residence) --
echo "--- 1. relay forwarding RTT vs offered load (REAL measurement) ---"
start_relay "PKERNEL_RELAY_PROBE_STAMP=1"
"$HERE/measure" rtt "$PORT" || fails=$((fails+1))
stop_relay
echo

# --- 2. end-to-end energy proxy ---------------------------------------------
echo "--- 2. per-message energy proxy, end-to-end (REAL bytes) ---"
start_relay ""
"$HERE/measure" energy "$PORT" "$ENERGY_M" "$ENERGY_P" || fails=$((fails+1))
stop_relay
echo

# --- 3. non-destructive: relay 6/6 still green with the flag OFF ------------
# (probe-stamp is NOT exported here, so the relay the suite spawns runs with the
#  flag off; `make -C` runs the recipe with cwd=relay so its ./relay resolves.)
echo "--- 3. non-destructive proof: relay 6/6 with probe-stamp OFF ---"
if make -C "$RELAY_DIR" test >/tmp/measure_6of6.log 2>&1; then
    grep -aE '\[relay-test\]' /tmp/measure_6of6.log | tail -1
    echo "    relay 6/6: PASS"
else
    tail -3 /tmp/measure_6of6.log; echo "    relay 6/6: FAIL"; fails=$((fails+1))
fi
rm -f /tmp/measure_6of6.log
echo

echo "=== summary: $fails failure(s) ==="
[ "$fails" -eq 0 ] && echo "ALL PASS: §4 latency+energy measured on the real relay; wire change additive."
exit "$fails"
