#!/bin/bash
# ---------------------------------------------------------------------------
# Wave 8 ① — 記憶→思考: the forward pass reads p-fs, measured 3 ways.
#
# §9 critique: "a library that only stores is not a thinking organ". This
# demo proves the wiring is real by showing accuracy that CANNOT exist
# without the forward pass reading memory out of p-fs:
#
#   (a) weights only   : node 1 trains (300 epochs, analytic backprop),
#                        retrieval OFF              -> ~95% / 100%
#   (b) memory only    : node 2 NEVER trains (random weights). It receives
#                        the engram block 'dtr/engrams' through p-fs P1
#                        replication + P2 ref gossip from node 1, and its
#                        forward blends top-k=3 engram votes into the
#                        logits -> far above chance (33%) from the swarm's
#                        memory alone.
#   (c) weights+memory : node 1, retrieval ON      -> >= (a) on both splits
#
# Both [ret off] and [ret ON] lines come from the same `dtr eval` — no
# cherry-picking. Node 2's (b) is the 2-node capstone: a brain that never
# learned anything classifies with the memory of the swarm.
#
# Exits non-zero if any condition fails.
# Usage:   ./run_memory_thought.sh
# Watch:   /tmp/w8mt_node1.log  /tmp/w8mt_node2.log  /tmp/w8mt_relay.log
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
export PKERNEL_RELAY_PORT=7424

PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

echo "[demo] starting relay on :$PKERNEL_RELAY_PORT"
"$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" -v >/tmp/w8mt_relay.log 2>&1 & PIDS+=($!)
sleep 1

echo "[demo] starting node 2 (NEVER trains — thinks with the swarm's memory)"
{ sleep 38; echo "dtr eval"; sleep 6; echo "exit"; } | \
  PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 "$BOOT/p-kernel" >/tmp/w8mt_node2.log 2>&1 & PIDS+=($!)
sleep 2

echo "[demo] starting node 1 (trainer): train -> remember -> eval (off+ON)"
{
  sleep 8                      # let SWIM mesh the two nodes
  echo "dtr train"             # (a)'s weights: real SGD, analytic backprop
  sleep 4
  echo "dtr remember"          # engrams -> p-fs 'dtr/engrams' (P1+P2 gossip)
  sleep 2
  echo "dtr eval"              # prints (a) [ret off] and (c) [ret ON]
  sleep 26                     # let chunks + ref gossip reach node 2
  echo "exit"
} | PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 "$BOOT/p-kernel" >/tmp/w8mt_node1.log 2>&1
wait "${PIDS[@]:1}" 2>/dev/null

# --------------------------------------------------------------------------
# parse: held-out + train accuracy per condition (last matching line wins)
# --------------------------------------------------------------------------
acc() { # $1=log $2=split-regex $3=mode-regex -> "95.0" or ""
    grep -E "eval $2.*\[$3\]" "$1" | tail -1 | sed -E 's/.*acc ([0-9.]+)%.*/\1/'
}
A_TR=$(acc /tmp/w8mt_node1.log "train   " "ret off");  A_HO=$(acc /tmp/w8mt_node1.log "held-out" "ret off")
C_TR=$(acc /tmp/w8mt_node1.log "train   " "ret ON");   C_HO=$(acc /tmp/w8mt_node1.log "held-out" "ret ON")
B0_TR=$(acc /tmp/w8mt_node2.log "train   " "ret off"); B0_HO=$(acc /tmp/w8mt_node2.log "held-out" "ret off")
B_TR=$(acc /tmp/w8mt_node2.log "train   " "ret ON");   B_HO=$(acc /tmp/w8mt_node2.log "held-out" "ret ON")

echo
echo "===== node 1 (trainer) — raw eval lines ====="
grep -E "\[dtr\] eval|\[ret\]|\[pfs\] saved" /tmp/w8mt_node1.log
echo
echo "===== node 2 (never trained) — raw eval lines ====="
grep -E "\[dtr\] eval|\[ret\]|\[pfs\] (ref|got block)" /tmp/w8mt_node2.log
echo
echo "==================== 記憶→思考: 3条件の精度表 ===================="
printf "  %-34s %10s %10s\n" "condition" "train" "held-out"
printf "  %-34s %9s%% %9s%%\n" "(a) weights only      (n1, ret off)" "${A_TR:-??}" "${A_HO:-??}"
printf "  %-34s %9s%% %9s%%   <- untrained baseline\n" "    random weights    (n2, ret off)" "${B0_TR:-??}" "${B0_HO:-??}"
printf "  %-34s %9s%% %9s%%   <- swarm memory only\n"  "(b) memory only       (n2, ret ON)"  "${B_TR:-??}"  "${B_HO:-??}"
printf "  %-34s %9s%% %9s%%\n" "(c) weights + memory  (n1, ret ON)"  "${C_TR:-??}"  "${C_HO:-??}"
echo "==================================================================="

fail=0
ck() { # $1=desc $2=cond(awk expr over a,b)
    if [ -z "${3:-}" ] || [ -z "${4:-}" ]; then echo "[FAIL] $1 (missing data)"; fail=1; return; fi
    if awk -v a="$3" -v b="$4" "BEGIN{exit !($2)}"; then echo "[OK]   $1"; else echo "[FAIL] $1"; fail=1; fi
}
ck "(a) trained weights >= 90% held-out"                 "a >= 90"  "${A_HO:-}" 0
ck "(b) memory-only beats chance by far (held-out>=55%)" "a >= 55"  "${B_HO:-}" 0
ck "(b) memory-only >> its own untrained baseline"       "a >= b+20" "${B_HO:-}" "${B0_HO:-}"
ck "(c) >= (a) on held-out"                              "a >= b"   "${C_HO:-}" "${A_HO:-}"
ck "(c) >= (a) on train"                                 "a >= b"   "${C_TR:-}" "${A_TR:-}"
grep -q "engrams loaded from p-fs" /tmp/w8mt_node2.log \
    && echo "[OK]   node 2's engrams came FROM p-fs (gossip), not local state" \
    || { echo "[FAIL] node 2 never loaded engrams from p-fs"; fail=1; }

echo
if [ "$fail" -eq 0 ]; then
    echo "[demo] PASS — the forward pass demonstrably thinks with p-fs memory"
else
    echo "[demo] FAIL — see /tmp/w8mt_node{1,2}.log"
fi
exit "$fail"
