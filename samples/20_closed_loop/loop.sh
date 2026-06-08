#!/bin/bash
# ---------------------------------------------------------------------------
# Wave 12 — 鎖を環にする (close the loop):
#   思考の結果が行動になり、行動が知覚を変え、経験が学習を産んで反射へ戻る。
#
# philosophy-gap-audit-2 の核心: 部品は緑だが「閉じた制御ループではなく、
# 個別に緑の鎖」。3つの切れ目を閉じたことを *数で* 証明する:
#
#   (a) G17 — 行動の片肺を閉じる: §7 MoE ゲートで「最も効く専門家」が出した
#       結論も §8 反射層を叩く。これまで reflex は dtr 経路だけだった。
#       => `moe <critical>` の推論完了点から [reflex] FIRE が出ることを示す。
#       => 通常 class (normal) では reflex は黙る (デフォルト有効でも無動作)。
#
#   (b) 負帰還を「測れる形」で閉じる: 行動(CONSERVE)が知覚(pressure)を変え、
#       §7 ゲートが負荷を再分配して脅威を鎮める「一周」。外乱(脅威 burst)を
#       入れ、ループ有り(reflex on=行動が知覚へ戻る)では指標が *減衰して定常へ
#       収束* し、ループ無し(feedforward=行動が知覚へ戻らない)では脅威が滞留
#       し続けることを、脅威 dwell / 整定時間 / 残留分散 / 再励起回数 で示す。
#       => `reflex test` の [reflex-fb] 行 (loop ON vs OFF の数値表)。
#
#   (c) G18 — 熟慮→学習→反射の環: 熟慮層 (遅い時定数) が蓄積した経験(脅威
#       dwell)から CONSERVE の効き(learned_conserve)を学習で書き換え、外部
#       `dtr train` 無しに反射の振る舞いが時間とともに改善することを示す。
#       => `reflex test` の [reflex-learn] 行 (dwell が episode で減衰)。
#
# 計算はすべてカーネル内 (`reflex test` -> arch/common/reflex.c) で決定論的に
# 走り、§7 ゲートの効用は本番 moe_expert_utility をそのまま使う (重複定義
# なし)。単一ノードで完結する (relay/net 不要) ので毎回ビット同一。
#
# Usage:  ./loop.sh
# Exit:   0 = 環が閉じている (G17 配線 + 負帰還収束 + 熟慮学習); 非0 で失敗。
# Log:    /tmp/closed_loop_<arch>.log
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux";        ARCH=aarch64 ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64"; ARCH=x86_64  ;;
    *) echo "unsupported host arch $(uname -m)"; exit 1 ;;
esac

[ -x "$BOOT/p-kernel" ] || make -C "$BOOT" >/dev/null || { echo "build failed"; exit 1; }

LOG=/tmp/closed_loop_${ARCH}.log

# Drive a single node through the closed-loop demo. No net: moe_infer takes the
# local path (drpc_my_node=0xFF) so the G17 hook fires from the gate's own
# inference completion; `reflex test` is pure-local deterministic math.
#
# G38 (wave 17): the moe->reflex hook is now gated by the LEARNED model's REAL
# max-softmax confidence (was a dead 0xFF that always fired). The critical input
# only reaches the reflex once the model is trained and CONFIDENT it is class 2
# — thinking changes guarding. We train first; an UNTRAINED model is (correctly)
# not confident and would NOT fire — that is the whole point of G38.
{
    echo "dtr train 200"      # G38: make the model confident so it can guard
    echo "moe 120 5 0 90"     # (a) critical input -> learned class 2 (confident) -> reflex FIRE
    echo "moe 10 80 30 10"    # (a) normal input   -> learned class 0             -> reflex silent
    echo "reflex test"        # (b)+(c) closed-loop negative-feedback + learning
    echo "moe test"           # regression: §7/§8 property tests still green
    echo "exit"
} | timeout 90 "$BOOT/p-kernel" >"$LOG" 2>&1

echo "===================================================================="
echo " Wave 12 — closed control loop (chain -> ring)   [$ARCH]"
echo "===================================================================="

fail=0
pass() { echo "  PASS  $1"; }
bad()  { echo "  FAIL  $1"; fail=1; }

# --- (a) G17: moe inference completion drives the reflex layer --------------
echo
echo "(a) G17  thought(MoE expert) -> action(reflex):"
# The critical `moe` must be followed by a reflex FIRE; the normal one must not.
crit_gate=$(grep -c "\[moe\] gate=2" "$LOG")
fire=$(grep -c "\[reflex\] FIRE class=2" "$LOG")
grep -E "\[moe\] gate=2|\[reflex\] FIRE class=2" "$LOG" | sed 's/^/    /'
if [ "$crit_gate" -ge 1 ] && [ "$fire" -ge 1 ]; then
    pass "moe_infer (critical) reached the reflex layer (G17 wired)"
else
    bad  "moe_infer did not drive reflex (gate2=$crit_gate fire=$fire)"
fi

# --- (b) negative feedback converges; feedforward does not ------------------
echo
echo "(b) negative feedback (action->perception->gate) vs feedforward:"
grep -E "\[reflex-fb\]  loop (ON|OFF)" "$LOG" | sed 's/^/    /'
on=$(grep "\[reflex-fb\]  loop ON"  "$LOG")
off=$(grep "\[reflex-fb\]  loop OFF" "$LOG")
get() { echo "$1" | sed -n "s/.*$2=\([-0-9]*\).*/\1/p"; }
d_on=$(get "$on" dwell);   d_off=$(get "$off" dwell)
s_on=$(get "$on" settle);  s_off=$(get "$off" settle)
i_on=$(get "$on" iae);     i_off=$(get "$off" iae)
if [ -n "$d_on" ] && [ -n "$d_off" ] && \
   [ "$d_off" -gt "$d_on" ] && [ "$s_off" -gt "$s_on" ] && [ "$i_off" -gt "$i_on" ]; then
    pass "loop ON damps disturbance (dwell $d_off->$d_on, settle $s_off->$s_on, iae $i_off->$i_on)"
else
    bad  "feedforward not distinguishable from closed loop"
fi
if grep -q "\[reflex-fb\] PASS" "$LOG"; then
    pass "[reflex-fb] convergence asserted in-kernel"
else
    bad  "[reflex-fb] in-kernel assertion failed"
fi

# --- (c) deliberation learns from experience; behaviour improves ------------
echo
echo "(c) deliberation -> learning -> reflex (auto-adapt, no 'dtr train'):"
grep -E "\[reflex-learn\] (learn:|ep|PASS)" "$LOG" | sed 's/^/    /'
lline=$(grep "\[reflex-learn\] learn:" "$LOG")
lfirst=$(echo "$lline" | sed -n 's/.*learn: dwell \([0-9]*\)->\([0-9]*\) .*/\1/p')
llast=$(echo  "$lline" | sed -n 's/.*learn: dwell \([0-9]*\)->\([0-9]*\) .*/\2/p')
if [ -n "$lfirst" ] && [ -n "$llast" ] && [ "$llast" -lt "$lfirst" ]; then
    pass "experience improved reflex over time (dwell $lfirst -> $llast)"
else
    bad  "deliberation did not improve the indicator (dwell $lfirst -> $llast)"
fi
if grep -q "\[reflex-learn\] PASS" "$LOG"; then
    pass "[reflex-learn] learning asserted in-kernel"
else
    bad  "[reflex-learn] in-kernel assertion failed"
fi

# --- regression: the in-kernel property suites stay green -------------------
echo
echo "(regression) in-kernel property suites:"
if grep -q "\[reflex-test\] ALL PASS" "$LOG"; then pass "reflex test ALL PASS"
else bad "reflex test not ALL PASS"; fi
if grep -q "\[moe-test\] ALL PASS" "$LOG"; then pass "moe test ALL PASS"
else bad "moe test not ALL PASS"; fi

echo
echo "--------------------------------------------------------------------"
if [ "$fail" -eq 0 ]; then
    echo " RESULT: PASS — the loop is closed (thought->action->perception->"
    echo "         learning->reflex). Full log: $LOG"
    exit 0
else
    echo " RESULT: FAIL — see $LOG"
    exit 1
fi
