#!/bin/bash
# ---------------------------------------------------------------------------
# N-2d — LIVE measured-capability supernode auto-promotion cert (SUPER-AUTO-LIVE).
#
# Proves a REAL booted ./p-kernel node that MEASURES itself reachable + public/
# cone AUTO-PROMOTES its own supernode capability bit (no PKERNEL_SUPERNODE) and
# is then ELECTED the region supernode by the unchanged NOCENTRAL selector — and
# that a SYMMETRIC-NAT node (unpunchable) NEVER advertises the bit nor is elected.
# Mirrors samples/11_distributed/run_relay_autofallback_live.sh.
#
# Setup uses a private user+net namespace (`unshare -rn`): inside it we are root
# in our own netns, bring up lo, run the REFL1-aware relay(s) on loopback, and
# boot AUTO nodes (PKERNEL_RELAY set, NO PKERNEL_SUPERNODE). On loopback the
# node's single socket presents the SAME source port to every relay vantage
# point -> CONE (endpoint-independent) -> fitness-good -> auto-promote.
#
# Sub-certs (all load-bearing), per supernode-autopromote.md §E.2:
#   PASS  : a public/cone node logs `autopromote: PROMOTE` (>=1) and, after the
#           bit gossips, `snf stat` shows my_supernode=<that node> on ITSELF and
#           the SAME id on a higher-id peer (peers agree, NOCENTRAL).
#   TEETH : a SYMMETRIC-emulated node (a second vantage point reached via a
#           source-port-rewriting socat proxy so the two relays observe
#           DIFFERENT external ports) logs ZERO `autopromote: PROMOTE` and is
#           NEVER shown as my_supernode by any peer — disambiguates "promotion
#           selects on measured reachability" from "any node with a relay
#           promotes."
#
# CAPABILITY: needs a net-namespace / NET_ADMIN substrate. Where it is absent
# (e.g. this aarch64 PRoot host, where `unshare -rn` returns EINVAL) the row
# SKIPs CLEANLY (exit 0) — the in-proc cert (tests/run_autopromote.sh) is the
# always-runnable gate, exactly as the connect-anywhere live rows defer.
#
# Usage:   ./run_supernode_autopromote_live.sh
# Watch:   /tmp/pksap_*.log
# ---------------------------------------------------------------------------
set -u

# ---- capability probe FIRST: a private net namespace with NET_ADMIN --------
if ! unshare -rn true 2>/dev/null; then
    echo "[autopromote-live] SKIP (no netns/NET_ADMIN — in-proc cert is the gate)"
    exit 0
fi
if ! command -v socat >/dev/null 2>&1; then
    echo "[autopromote-live] SKIP (no socat — in-proc cert is the gate)"
    exit 0
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"          # .../p-kernel

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "[autopromote-live] SKIP (unsupported host arch $(uname -m))"; exit 0 ;;
esac

# Build OUTSIDE the namespace (the netns has loopback only).
[ -x "$BOOT/p-kernel" ]    || make -C "$BOOT"        >/dev/null 2>&1 || { echo "[autopromote-live] SKIP (kernel build failed)"; exit 0; }
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay"  >/dev/null 2>&1 || { echo "[autopromote-live] SKIP (relay build failed)"; exit 0; }

# Re-exec the heavy body INSIDE a fresh user+net namespace.
if [ "${SAP_INSIDE:-0}" != "1" ]; then
    exec env SAP_INSIDE=1 BOOT="$BOOT" ROOT="$ROOT" unshare -rn bash "$0"
fi

# ====================== inside the private netns ===========================
ip link set lo up 2>/dev/null || { echo "[autopromote-live] SKIP (cannot bring up lo in netns)"; exit 0; }

KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
export PKERNEL_RELAY_KEY="$KEY"

PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

# Boot one AUTO node (NO PKERNEL_SUPERNODE). $1=id $2=log $3=relay-spec.
# Lets the bit settle through SWIM, prints the supernode view, quits.
boot_node() {
    local id="$1" log="$2" spec="$3"
    { sleep 75; echo "snf stat"; sleep 2; echo nodes; sleep 1; echo exit; } \
        | PKERNEL_NODE_ID="$id" PKERNEL_AUTONET=1 PKERNEL_RELAY="$spec" \
          "$BOOT/p-kernel" >"$log" 2>&1
}

# =====================================================================
# PASS  — two loopback relays (two vantage points), same source port -> CONE
# =====================================================================
P1=7481; P2=7482
echo "[PASS] cone node (relays :$P1,:$P2) must auto-promote and be elected"
"$ROOT/relay/relay" -p "$P1" -v >/tmp/pksap_relay1.log 2>&1 & PIDS+=($!)
"$ROOT/relay/relay" -p "$P2" -v >/tmp/pksap_relay2.log 2>&1 & PIDS+=($!)
sleep 1
SPEC="127.0.0.1:$P1,127.0.0.1:$P2"
( boot_node 1 /tmp/pksap_n1.log "$SPEC" ) &
sleep 3
( boot_node 2 /tmp/pksap_n2.log "$SPEC" ) &
wait
sleep 1

N1_PROMO=$(grep -c 'autopromote: PROMOTE' /tmp/pksap_n1.log)
# node 1 is the lowest id; it should elect ITSELF (my_supernode=0, internal idx).
N1_SELF=$(grep -c 'my_supernode=0' /tmp/pksap_n1.log)
N2_AGREE=$(grep -c 'my_supernode=0' /tmp/pksap_n2.log)
echo "    node1 PROMOTE: $N1_PROMO (expect >=1)  node1 my_supernode=0: $N1_SELF  node2 agrees=0: $N2_AGREE"

PASS=0
[ "$N1_PROMO" -ge 1 ] && [ "$N1_SELF" -ge 1 ] && [ "$N2_AGREE" -ge 1 ] && PASS=1

# =====================================================================
# TEETH — symmetric-emulated node: a second vantage point reached via a
# source-port-rewriting socat proxy so the two relays observe DIFFERENT
# external ports -> SYMMETRIC -> never promotes, never elected.
# =====================================================================
P3=7483; PROXY=7484
echo
echo "[TEETH] symmetric node (relay :$P3 + socat-rewritten :$PROXY) must NOT promote/elect"
"$ROOT/relay/relay" -p "$P3" -v >/tmp/pksap_relay3.log 2>&1 & PIDS+=($!)
# socat forwards :$PROXY -> :$P3 with its OWN source port, so relay :$P3 sees a
# DIFFERENT external port for the proxied path than for the direct path.
socat UDP4-RECVFROM:"$PROXY",fork UDP4-SENDTO:127.0.0.1:"$P3" >/dev/null 2>&1 & PIDS+=($!)
sleep 1
SYMSPEC="127.0.0.1:$P3,127.0.0.1:$PROXY"
( boot_node 1 /tmp/pksap_sym1.log "$SYMSPEC" ) &
sleep 3
( boot_node 2 /tmp/pksap_sym2.log "$SYMSPEC" ) &
wait
sleep 1

SYM_PROMO=$(grep -c 'autopromote: PROMOTE' /tmp/pksap_sym1.log)
SYM_ELECT=$(grep -c 'my_supernode=0' /tmp/pksap_sym2.log)
echo "    symnode PROMOTE: $SYM_PROMO (expect 0)   peer elects it (my_supernode=0): $SYM_ELECT (expect 0)"

TEETH=0
[ "$SYM_PROMO" -eq 0 ] && [ "$SYM_ELECT" -eq 0 ] && TEETH=1

# =====================================================================
echo
echo "===================================================================="
echo "  PASS  (cone node auto-promotes + elected) : $([ $PASS  = 1 ] && echo PASS || echo FAIL)"
echo "  TEETH (symmetric node never promotes)     : $([ $TEETH = 1 ] && echo PASS || echo FAIL)"
echo "===================================================================="
if [ $PASS = 1 ] && [ $TEETH = 1 ]; then
    echo "[autopromote-live] PASS"
    exit 0
fi
echo "[autopromote-live] FAIL"
exit 1
