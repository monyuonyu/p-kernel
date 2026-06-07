#!/bin/bash
# =============================================================================
# run.sh — two-layer latency against the REAL relay (survival-network.md §8/§10-B)
# wave-15 B隊 / 俯瞰監査 G31: §4 の核 (latency/light-speed) を「数」で測る
# -----------------------------------------------------------------------------
# §10 ステップB を逐語で:
#   「ノード間通信に遅延(距離に比例)を導入。反射層: 近傍のみ・低遅延・即応。
#     熟慮層: 全体・高遅延・深い判断/更新。
#     *遅延が反射層の即応性を損なわないことを確認*。」
#
# これを **REAL ./relay** に対して実測する。relay の距離→遅延ノブを ON にする:
#   RELAY_FAR_NODES=3        node 3 (= far / 熟慮層) 宛の転送だけを遅らせる
#   RELAY_FAR_DELAY_MS=300   その遅延量 (= 光速の壁の代理。下の honesty 参照)
#   (near 宛 = 遅延 0。既存テストはノブ未設定=遅延 0 で不変)
#
# latency_client は同一プロセスで 3 ソケット (ship=1 / near=2 / far=3) を握り、
# ship から near/far へ往復プローブを撃つ。決定的なテストは NON-INTERFERENCE:
#   far プローブを 1 発撃ち、それが relay の遅延キューに居る *間に* near プローブを
#   連射する。二層が本当に分離しているなら near は全て即座に往復し、far より先に
#   返ってくる (relay が near を far の後ろで head-of-line ブロックしない)。
#
# 何がモデルで何が実測か (正直に):
#   モデル : far_delay=300ms という「距離 ∝ 光速遅延」の値。実距離・実無線ではない。
#            (火星片道は ~3..22 分。300ms は localhost で観測可能なスケールへ圧縮した代理)
#   実測   : リアルな UDP socket 往復遅延を REAL relay 経由で測る。near が far の遅延に
#            引きずられない (即応性が保たれる) ことは *実測* で示す。
#
# Usage: ./run.sh   (REPEAT 回まわして RESULT 行を出す。考察は docs/benchmarks/latency.md)
# =============================================================================
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
RELAY_DIR="$ROOT/relay"

PORT="${PORT:-27440}"
FAR_DELAY_MS="${FAR_DELAY_MS:-300}"
REPEAT="${REPEAT:-3}"

export PKERNEL_RELAY_KEY="${PKERNEL_RELAY_KEY:-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef}"

# Build the real relay and the measurement client.
make -C "$RELAY_DIR" relay >/dev/null 2>&1 || { echo "relay build failed"; exit 1; }
cc -Wall -Wextra -O2 -std=gnu11 -I"$RELAY_DIR" \
    -o "$HERE/latency_client" "$HERE/latency_client.c" "$RELAY_DIR/sha256.c" \
    || { echo "client build failed"; exit 1; }

cleanup() { [ -n "${RELAY_PID:-}" ] && kill "$RELAY_PID" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

echo "=== 29_latency: two-layer latency vs REAL relay (§8 / §10-B) ==="
echo "    port=$PORT  far_delay=${FAR_DELAY_MS}ms  repeats=$REPEAT"
echo "    relay knob: RELAY_FAR_NODES=3 RELAY_FAR_DELAY_MS=$FAR_DELAY_MS (near=0)"
echo

fails=0
for r in $(seq 1 "$REPEAT"); do
    LOG="$(mktemp /tmp/latency_relay.XXXXXX)"
    RELAY_FAR_NODES=3 RELAY_FAR_DELAY_MS="$FAR_DELAY_MS" \
        "$RELAY_DIR/relay" -p "$PORT" >"$LOG" 2>&1 &
    RELAY_PID=$!
    sleep 1

    echo "--- run $r/$REPEAT ---"
    "$HERE/latency_client" "$PORT" "$FAR_DELAY_MS"
    rc=$?
    [ "$rc" -ne 0 ] && fails=$((fails + 1))

    kill "$RELAY_PID" 2>/dev/null; wait "$RELAY_PID" 2>/dev/null; RELAY_PID=""
    rm -f "$LOG"
    echo
done

echo "=== summary: $((REPEAT - fails))/$REPEAT runs PASS ==="
[ "$fails" -eq 0 ] && echo "ALL PASS: §8 two-layer immediacy preserved under injected far latency."
exit "$fails"
