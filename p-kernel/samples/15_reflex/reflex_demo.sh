#!/bin/bash
# ---------------------------------------------------------------------------
# Wave 9 ② — 思考→行動の配線: inference result -> §8 reflex layer action.
#
# 第三レビューの配線②: "推論結果が class ラベルを吐いて、どこにも行かない。
# 『守る力』は計算するが、目の前の一点を守る(行動する)手が無い。" This demo
# proves the wiring is real by showing the dtr inference completion drive
# *real system operations*, not decorative prints:
#
#   node1 (attacked): a stream of "critical" (class 2) sensor inputs makes
#     reflex_on_inference fire the action table:
#       BEACON   -> publishes "reflex/alarm/0" (a neighbour can hear it)
#       CONSERVE -> raises this node's gossiped pressure (moe gate routes away)
#       SHIELD   -> after >=2 consecutive criticals; while SHIELDed a `selfc`
#                   germination is REFUSED (no unknown code intake under attack)
#   node2 (neighbour): hears the alarm and reacts with its OWN judgement — an
#     *attenuated* reflex (CONSERVE only, never SHIELD; relay hop decays to 0)
#     so the swarm does not convulse in unison (§8 local closed loop).
#   stop the input -> after the §8 hysteresis (5s) both nodes auto-release.
#
# Two nodes mesh in REDUCED mode, so `infer` runs tensor-parallel and the
# reflex hook fires from the dtr completion point — no central commander
# anywhere (alarms are information, not orders).
#
# Exits non-zero if any assertion fails.
# Usage:  ./reflex_demo.sh
# Watch:  /tmp/reflex_node1.log  /tmp/reflex_node2.log  /tmp/reflex_relay.log
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "unsupported host arch $(uname -m)"; exit 1 ;;
esac

[ -x "$BOOT/p-kernel" ]    || make -C "$BOOT"       >/dev/null || exit 1
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay" >/dev/null || exit 1

export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
export PKERNEL_RELAY_HOST=127.0.0.1
export PKERNEL_RELAY_PORT=7466

N1=/tmp/reflex_node1.log
N2=/tmp/reflex_node2.log
RL=/tmp/reflex_relay.log

PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

echo "[demo] starting relay on :$PKERNEL_RELAY_PORT"
"$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" >"$RL" 2>&1 & PIDS+=($!)
sleep 1

echo "[demo] starting node 2 (neighbour — reacts with its own attenuated reflex)"
{ sleep 55; echo "exit"; } | \
  PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 "$BOOT/p-kernel" >"$N2" 2>&1 & PIDS+=($!)
sleep 2

echo "[demo] starting node 1 (attacked — critical inputs -> reflex action)"
{
  sleep 10                       # let SWIM mesh the two nodes (REDUCED)
  echo "dtr train"               # real SGD so 'critical' input -> class 2
  sleep 6
  echo "dist"                    # show degrade level (REDUCED, 2 nodes)
  sleep 1
  for i in 1 2 3 4 5; do         # 連続注入: 5 critical inferences
      echo "infer 120 5 0 90"    # high temp -> class 2 (critical)
      sleep 1
  done
  echo "selfc demo"             # SHIELD must REFUSE this (real defence)
  sleep 1
  echo "reflex stat"            # snapshot: SHIELD/CONSERVE ACTIVE
  sleep 8                        # > 5s hysteresis -> auto-release
  echo "reflex stat"            # snapshot: released
  echo "exit"
} | PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 "$BOOT/p-kernel" >"$N1" 2>&1
wait "${PIDS[@]:1}" 2>/dev/null

# --------------------------------------------------------------------------
echo
echo "===== node 1 (attacked) — reflex fire / shield / release ====="
grep -nE "TP\(REDUCED\): class|DKVA class|\[reflex\]" "$N1" | \
    grep -vE "open  topic|initialized" | sed -E 's/^[0-9]+://'
echo
echo "===== node 2 (neighbour) — propagation + attenuation + release ====="
grep -nE "\[reflex\] (heard|CONSERVE|SHIELD|FIRE)" "$N2" | sed -E 's/^[0-9]+://'
echo

# --------------------------------------------------------------------------
fail=0
ck() { # $1=desc  $2=0/1 result
    if [ "$2" -eq 1 ]; then echo "[OK]   $1"; else echo "[FAIL] $1"; fail=1; fi
}
has() { grep -qE "$2" "$1"; }   # has <file> <regex> -> rc
cnt() { grep -cE "$2" "$1"; }   # cnt <file> <regex>

echo "==================== 思考→行動: 反射の配線アサート ===================="

# (1) self-observed danger fires the FULL reflex on the attacked node
has "$N1" "\[reflex\] FIRE class=2 ->.*SHIELD.*CONSERVE.*BEACON" && r=1 || r=0
ck "node1: critical inference FIRES SHIELD+CONSERVE+BEACON (real action table)" "$r"

# (2) BEACON actually published (a neighbour can hear it)
[ "$(cnt "$N1" "\[reflex\] BEACON class=2")" -ge 2 ] && r=1 || r=0
ck "node1: BEACON published to reflex/alarm (>=2 times)" "$r"

# (3) SHIELD is a REAL operation: it refuses selfc germination
has "$N1" "SHIELD active . refusing new selfc germination" && r=1 || r=0
ck "node1: SHIELD blocks new selfc germination (no unknown code under attack)" "$r"

# (4) the alarm propagates to the neighbour, which reacts attenuated
has "$N2" "heard alarm from node0 class=2" && r=1 || r=0
ck "node2: hears the alarm (BEACON propagated, not central)" "$r"
has "$N2" "attenuated CONSERVE .no SHIELD" && r=1 || r=0
ck "node2: reacts with attenuated CONSERVE (its own judgement)" "$r"

# (5) the neighbour does NOT fully convulse: never self-FIREs, never SHIELDs
[ "$(cnt "$N2" "\[reflex\] FIRE")" -eq 0 ] && r=1 || r=0
ck "node2: never self-FIREs (only the attacked node SHIELDs — no convulsion)" "$r"

# (6) §8 hysteresis: after input stops, both actions auto-release (~5s)
has "$N1" "SHIELD released .hysteresis" && r=1 || r=0
ck "node1: SHIELD auto-releases after 5s hysteresis (slow-out, no oscillation)" "$r"
has "$N1" "CONSERVE released .hysteresis" && r=1 || r=0
ck "node1: CONSERVE auto-releases after 5s hysteresis" "$r"
has "$N2" "CONSERVE released .hysteresis" && r=1 || r=0
ck "node2: CONSERVE auto-releases after 5s hysteresis" "$r"

echo "======================================================================"
echo
if [ "$fail" -eq 0 ]; then
    echo "[demo] PASS — inference drives real, local, decaying defence (思考に手足が付いた)"
else
    echo "[demo] FAIL — see $N1 / $N2"
fi
exit "$fail"
