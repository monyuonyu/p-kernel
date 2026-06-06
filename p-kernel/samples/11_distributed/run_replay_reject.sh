#!/bin/bash
# ---------------------------------------------------------------------------
# Wave 11: prove the relay client rejects REPLAYED frames.
#
# Companion to run_relay_forgery.sh. That test proved a bad-MAC frame is
# dropped (G4). This proves a captured-and-resent frame -- which carries a
# VALID MAC -- is ALSO dropped, by the receive-side per-source nonce window.
#
# replay_inject.py sends three distinct, valid frames (all must be ACCEPTED,
# zero replay drops) and then resends two of them verbatim (each must be
# DROPPED as a replay -> "[net_relay] replay drop n=...").
#
# Usage:  ./run_replay_reject.sh
# Exit 0 only if: a replay was dropped AND no fresh frame was misflagged.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "unsupported host arch $(uname -m)"; exit 1 ;;
esac
[ -x "$BOOT/p-kernel" ] || make -C "$BOOT" >/dev/null || exit 1

PORT=7411
KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
NODELOG=/tmp/pkreplay_node.log
INJLOG=/tmp/pkreplay_relay.log

PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

python3 "$HERE/replay_inject.py" "$PORT" "$KEY" >"$INJLOG" 2>&1 & PIDS+=($!)
sleep 1

# Drive the shell: let the injections land, then dump [rx-relay] counters.
{ sleep 8; echo "rx"; sleep 4; } | \
    PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 \
    PKERNEL_RELAY="127.0.0.1:$PORT" PKERNEL_RELAY_KEY="$KEY" \
    "$BOOT/p-kernel" >"$NODELOG" 2>&1 & PIDS+=($!)

# Wait for the `rx` command output (printed after the injections complete) so
# both the replay-drop log and the [rx-relay] counter line are present.
waited=0
while [ "$waited" -lt 30 ]; do
    if grep -q "rx-relay" "$NODELOG" 2>/dev/null; then break; fi
    sleep 0.5; waited=$((waited + 1))
done
sleep 1   # let the line flush fully

echo "----- inject log -----"; cat "$INJLOG" 2>/dev/null
echo "----- node replay-drop lines -----"; grep "replay drop" "$NODELOG" 2>/dev/null
echo "----- node [rx-relay] line -----"; grep "rx-relay" "$NODELOG" 2>/dev/null

RC=0

if ! grep -q "replay drop" "$NODELOG" 2>/dev/null; then
    echo "FAIL — node did NOT drop replayed frames (replay window regression)"
    RC=1
fi

# A fresh frame must never be misflagged: replay counter must be >0 (we sent
# replays) but must NOT exceed the number of replays we actually sent (2).
REPLAY=$(grep "rx-relay" "$NODELOG" 2>/dev/null | tail -1 \
         | sed -n 's/.*replay=\([0-9]*\).*/\1/p')
OKC=$(grep "rx-relay" "$NODELOG" 2>/dev/null | tail -1 \
      | sed -n 's/.*ok=\([0-9]*\).*/\1/p')
echo "----- parsed: ok=$OKC replay=$REPLAY -----"
if [ -n "$REPLAY" ]; then
    if [ "$REPLAY" -lt 1 ]; then
        echo "FAIL — replay counter is 0 despite replays sent"; RC=1
    elif [ "$REPLAY" -gt 2 ]; then
        echo "FAIL — replay counter $REPLAY > 2: a FRESH frame was misflagged"
        RC=1
    fi
fi
# At least the three fresh frames must have been accepted.
if [ -n "$OKC" ] && [ "$OKC" -lt 3 ]; then
    echo "FAIL — only $OKC fresh frames accepted (expected >=3): false drop"
    RC=1
fi

if [ "$RC" -eq 0 ]; then
    echo "PASS — replays dropped, fresh frames all accepted (no false drop)"
fi
exit "$RC"
