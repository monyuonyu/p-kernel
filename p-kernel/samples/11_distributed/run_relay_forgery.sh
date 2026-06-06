#!/bin/bash
# ---------------------------------------------------------------------------
# G4 (wave 10): prove the relay client verifies inbound HMACs.
#
# A malicious relay (forge_inject.py) sends v2 frames with bogus MACs from
# the relay's own UDP tuple. A correct node MUST drop them and log
# "[net_relay] mac drop n=...". This asserts that drop happens.
#
# Usage:  ./run_relay_forgery.sh
# Exit 0 only if at least one forged frame was dropped.
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

PORT=7409
KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
NODELOG=/tmp/pkforge_node.log
FORGELOG=/tmp/pkforge_relay.log

PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

python3 "$HERE/forge_inject.py" "$PORT" "$KEY" >"$FORGELOG" 2>&1 & PIDS+=($!)
sleep 1

{ echo "help"; sleep 8; } | \
    PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 \
    PKERNEL_RELAY="127.0.0.1:$PORT" PKERNEL_RELAY_KEY="$KEY" \
    "$BOOT/p-kernel" >"$NODELOG" 2>&1 & PIDS+=($!)

# Give the node time to register + receive the injected frames.
waited=0
while [ "$waited" -lt 20 ]; do
    if grep -q "mac drop" "$NODELOG" 2>/dev/null; then break; fi
    sleep 0.5; waited=$((waited + 1))
done

echo "----- forge log -----"; cat "$FORGELOG" 2>/dev/null
echo "----- node mac-drop lines -----"; grep "mac drop" "$NODELOG" 2>/dev/null

if grep -q "mac drop" "$NODELOG" 2>/dev/null; then
    echo "PASS — forged frames were dropped by inbound HMAC verification"
    exit 0
fi
echo "FAIL — node did NOT drop forged frames (G4 regression)"
echo "----- node tail -----"; tail -15 "$NODELOG" 2>/dev/null
exit 1
