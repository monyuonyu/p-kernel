#!/bin/bash
# ---------------------------------------------------------------------------
# §3 self-regeneration — the swarm grows a NEW FULL CELL on an empty plate.
#
#   node 1 (full cell)     dtr train          -> real SGD, ~95%/100% accuracy
#                          dtr save           -> weights become the p-fs
#                                                versioned object "dtr/weights"
#                          selfc save genome.c-> the demo C source becomes the
#                                                p-fs object "genome.c"
#                          genome publish cell-> a fixed-width MANIFEST naming
#                                                {weights, code} is saved as
#                                                the named ref "genome/manifest"
#
#   node 2 (EMPTY plate)   never trains. Boots with PKERNEL_SPROUT=1 and
#                          auto-germinates: waits for "genome/manifest" to
#                          gossip in (p-fs P1 blocks + P2 refs over the
#                          relay), restores the weights through the dtr load
#                          core, compiles "genome.c" INSIDE its own kernel
#                          (libtcc/selfc), and reports:
#                              [genome] sprouted: a full cell grew from the
#                                                 swarm's DNA
#
# PASS requires, in node 2's log, ALL of:
#   1. the manifest arrived via gossip
#   2. the weights were restored from the p-fs replica
#   3. `dtr eval` returns EXACTLY the trained accuracy node 1 measured
#      (bit-identical float32 blob -> identical numbers, ~95% train /
#       ~100% held-out — node 2 never ran a single SGD step)
#   4. node 1's C source actually EXECUTED inside node 2's kernel
#   5. the sprouted banner
#
# Requires a libtcc-enabled build (apt-get install libtcc-dev). The script
# preflights this and refuses to fake a PASS without the code-execution leg.
#
# Usage:   ./sprout.sh
# Watch:   /tmp/genome_node{1,2}.log  /tmp/genome_relay.log
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "unsupported host arch $(uname -m)"; exit 1 ;;
esac

[ -x "$BOOT/p-kernel" ]    || make -C "$BOOT"        >/dev/null || exit 1
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay"  >/dev/null || exit 1

# preflight: the germination proof includes in-kernel compilation; a
# build without libtcc cannot deliver leg 4, so be honest and stop.
if printf 'selfc ls\nexit\n' | "$BOOT/p-kernel" 2>/dev/null | grep -q "no libtcc"; then
    echo "[demo] FAIL — this build has no libtcc; install libtcc-dev and"
    echo "       rebuild $BOOT (the code-execution leg cannot be proven)"
    exit 1
fi

export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
export PKERNEL_RELAY_HOST=127.0.0.1
export PKERNEL_RELAY_PORT=7416

PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

echo "[demo] starting relay on :$PKERNEL_RELAY_PORT"
"$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" -v >/tmp/genome_relay.log 2>&1 & PIDS+=($!)
sleep 1

echo "[demo] starting node 2 — an EMPTY plate (never trains, PKERNEL_SPROUT=1)"
{ sleep 55; echo "dtr eval"; sleep 3; echo "genome"; sleep 2; echo "exit"; } | \
  PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 PKERNEL_SPROUT=1 \
  "$BOOT/p-kernel" >/tmp/genome_node2.log 2>&1 & PIDS+=($!)
sleep 2

echo "[demo] starting node 1 — the full cell (train -> save -> code -> publish)"
{
  sleep 8                       # let SWIM mesh the two nodes
  echo "dtr train"              # real training; prints eval (~95%/100%)
  sleep 4
  echo "dtr save"               # the brain -> p-fs "dtr/weights"
  sleep 2
  echo "selfc save genome.c"    # the code  -> p-fs "genome.c"
  sleep 2
  echo "genome publish cell"    # the DNA index -> p-fs "genome/manifest"
  sleep 42                      # let gossip + germination on node 2 finish
  echo "exit"
} | PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 \
    "$BOOT/p-kernel" >/tmp/genome_node1.log 2>&1
wait "${PIDS[@]:1}" 2>/dev/null

echo
echo "===== node 1 (full cell) ====="
grep -aE "\[dtr\] (eval|trained|weights)|\[selfc\] demo source|\[genome\] published" /tmp/genome_node1.log
echo
echo "===== node 2 (empty plate -> full cell) ====="
grep -aE "\[genome\]|\[guard\] dtr recover|\[dtr\] eval|\[selfc" /tmp/genome_node2.log
echo

fail() { echo "[demo] FAIL — $1 (logs: /tmp/genome_node{1,2}.log /tmp/genome_relay.log)"; exit 1; }

# 1. the manifest reached node 2 via gossip
grep -aq "\[genome\] manifest arrived" /tmp/genome_node2.log \
    || fail "manifest never arrived on node 2"

# 2. the weights were restored from node 2's local p-fs replica
grep -aq "\[guard\] dtr recover: weights restored from p-fs" /tmp/genome_node2.log \
    || fail "weights were not restored on node 2"

# 3. node 2's eval == node 1's post-train eval (same blob -> same numbers),
#    and it is genuinely trained accuracy, not chance
N1_EVAL=$(grep -a "\[dtr\] eval" /tmp/genome_node1.log | tail -2 | tr -d '\r')
N2_EVAL=$(grep -a "\[dtr\] eval" /tmp/genome_node2.log | tail -2 | tr -d '\r')
[ -n "$N2_EVAL" ] || fail "node 2 produced no eval output"
[ "$N1_EVAL" = "$N2_EVAL" ] \
    || fail "node 2 eval differs from node 1's trained eval"
echo "$N2_EVAL" | grep -qE "acc (9[0-9]|100)\." \
    || fail "node 2 accuracy is not trained-level (expected ~95%/100%)"

# 4. node 1's code EXECUTED inside node 2's kernel (in-kernel libtcc)
grep -aq "compiled at runtime inside the kernel" /tmp/genome_node2.log \
    || fail "node 1's code did not run inside node 2"

# 5. the banner that says what just happened
grep -aq "\[genome\] sprouted: a full cell grew from the swarm's DNA" \
    /tmp/genome_node2.log || fail "node 2 never claimed full-cell status"

echo "[demo] PASS — the swarm grew a new armour plate into a full cell:"
echo "        weights (95%/100%), code (ran in-kernel) and role all came"
echo "        from gossip; node 2 never trained and never compiled a file."
