#!/bin/bash
# ---------------------------------------------------------------------------
# connect-anywhere SLICE 4 — LIVE auto relay-transport fallback cert (AUTOFB-LIVE).
#
# Proves a REAL booted ./p-kernel node on a UDP-BLOCKED net AUTOMATICALLY falls
# back to relay-TCP and joins the SAME relay mesh — WITHOUT any human setting
# PKERNEL_RELAY_TCP. The selector (arch/linux/*/net_dispatch.c::xport_auto_init)
# brings up relay-UDP, gets no answer (UDP blackholed), races relay-TCP after the
# head start, and adopts relay-tcp.
#
# UDP is blocked WITHOUT host privilege via a private user+net namespace
# (`unshare -rn`): inside it we are root in our own netns, bring up lo, run the
# relay (UDP+TCP on one port), and `iptables ... -p udp --dport $PORT -j DROP`
# (UDP blackholed to the relay; TCP intact).
#
# Sub-certs (all load-bearing), per docs/architecture/connect-anywhere.md 4S.c(2):
#   A. AUTO-FALLBACK : both nodes REGISTER over TCP, reach SWIM alive=2, and each
#                      log `auto: ... adopted relay-tcp`; NO node meshed over UDP
#                      (the block held -> zero non-tcp registrations).
#   TEETH 1          : block BOTH udp AND tcp -> NO join (no alive=2, no register)
#                      — proves a relay merely being up isn't what carries it.
#   TEETH 2          : same UDP-blocked netns but PKERNEL_RELAY_AUTOFALLBACK=0
#                      (force UDP-only) -> NO join, NO (tcp) registration —
#                      proves the AUTO selection (not the relay) carries the join.
#
# CAPABILITY: needs a net-namespace / NET_ADMIN substrate. Where it is absent
# (e.g. this aarch64 PRoot host, where `unshare -rn` returns EINVAL) the row
# SKIPs CLEANLY (exit 0) — the in-proc cert (tests/run_autofallback.sh) is the
# always-runnable gate, exactly as Slice 2/3's live rows defer.
#
# Usage:   ./run_relay_autofallback_live.sh
# Watch:   /tmp/pkafb_*.log
# ---------------------------------------------------------------------------
set -u

# ---- capability probe FIRST: a private net namespace with NET_ADMIN --------
if ! unshare -rn true 2>/dev/null; then
    echo "[autofallback-live] SKIP (no netns/NET_ADMIN — in-proc cert is the gate)"
    exit 0
fi
if ! command -v iptables >/dev/null 2>&1; then
    echo "[autofallback-live] SKIP (no iptables — in-proc cert is the gate)"
    exit 0
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"          # .../p-kernel

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "[autofallback-live] SKIP (unsupported host arch $(uname -m))"; exit 0 ;;
esac

# Build OUTSIDE the namespace (the netns has loopback only).
[ -x "$BOOT/p-kernel" ]    || make -C "$BOOT"        >/dev/null 2>&1 || { echo "[autofallback-live] SKIP (kernel build failed)"; exit 0; }
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay"  >/dev/null 2>&1 || { echo "[autofallback-live] SKIP (relay build failed)"; exit 0; }

# Re-exec the heavy body INSIDE a fresh user+net namespace. The marker env var
# stops infinite recursion; everything below the guard runs as root-in-userns
# with its own loopback-only netns.
if [ "${AUTOFB_INSIDE:-0}" != "1" ]; then
    exec env AUTOFB_INSIDE=1 BOOT="$BOOT" ROOT="$ROOT" unshare -rn bash "$0"
fi

# ====================== inside the private netns ===========================
ip link set lo up 2>/dev/null || { echo "[autofallback-live] SKIP (cannot bring up lo in netns)"; exit 0; }

KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
export PKERNEL_RELAY_KEY="$KEY"

PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

# Boot one node; AUTONET brings up SWIM at boot. $1=id $2=log [extra env applied
# by the caller's subshell]. Lets membership settle (auto adds up to ~2.5 s),
# prints nodes, quits.
boot_node() {
    local id="$1" log="$2"
    { sleep 12; echo nodes; sleep 2; echo exit; } \
        | PKERNEL_NODE_ID="$id" PKERNEL_AUTONET=1 "$BOOT/p-kernel" \
        >"$log" 2>&1
}

block_udp() {  # $1 = port
    iptables -A OUTPUT -p udp --dport "$1" -j DROP 2>/dev/null
    iptables -A INPUT  -p udp --dport "$1" -j DROP 2>/dev/null
}
block_tcp() {  # $1 = port
    iptables -A OUTPUT -p tcp --dport "$1" -j DROP 2>/dev/null
    iptables -A INPUT  -p tcp --dport "$1" -j DROP 2>/dev/null
}
flush_fw() { iptables -F 2>/dev/null; }

# =====================================================================
# A. AUTO-FALLBACK  (UDP blocked, TCP intact, NO PKERNEL_RELAY_TCP)
# =====================================================================
PORT_A=7471
echo "[A] auto-fallback — relay on :$PORT_A (UDP+TCP), UDP blackholed, auto selector"
flush_fw
"$ROOT/relay/relay" -p "$PORT_A" -v >/tmp/pkafb_relayA.log 2>&1 & PIDS+=($!)
sleep 1
block_udp "$PORT_A"
(
  export PKERNEL_RELAY="127.0.0.1:$PORT_A"
  PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 "$BOOT/p-kernel" </dev/null \
      >/tmp/pkafb_A_n2.log 2>&1 & echo $! >/tmp/pkafb_A_n2.pid
  sleep 2
  boot_node 1 /tmp/pkafb_A_n1.log
)
kill "$(cat /tmp/pkafb_A_n2.pid 2>/dev/null)" 2>/dev/null
sleep 1
flush_fw

A_tcp_regs=$(grep -c "registered.*(tcp)" /tmp/pkafb_relayA.log)
A_all_regs=$(grep -c "registered"        /tmp/pkafb_relayA.log)
A_udp_regs=$(( A_all_regs - A_tcp_regs ))
A_n1_alive=$(grep -c "alive=2"               /tmp/pkafb_A_n1.log)
A_n2_alive=$(grep -c "alive=2"               /tmp/pkafb_A_n2.log)
A_n1_auto=$(grep -c "auto: .* adopted relay-tcp" /tmp/pkafb_A_n1.log)
A_n2_auto=$(grep -c "auto: .* adopted relay-tcp" /tmp/pkafb_A_n2.log)

echo "    relay TCP registrations: $A_tcp_regs (expect >=2)   non-tcp regs: $A_udp_regs (expect 0)"
echo "    node1 auto->relay-tcp: $A_n1_auto   node2 auto->relay-tcp: $A_n2_auto"
echo "    node1 alive=2: $A_n1_alive   node2 alive=2: $A_n2_alive"
grep -E "registered" /tmp/pkafb_relayA.log | sed 's/^/      /'

A_PASS=0
[ "$A_tcp_regs" -ge 2 ] && [ "$A_udp_regs" -eq 0 ] \
    && [ "$A_n1_auto" -ge 1 ] && [ "$A_n2_auto" -ge 1 ] \
    && [ "$A_n1_alive" -ge 1 ] && [ "$A_n2_alive" -ge 1 ] && A_PASS=1

# =====================================================================
# TEETH 1  (block BOTH udp AND tcp -> no channel -> no join)
# =====================================================================
PORT_T1=7472
echo
echo "[TEETH 1] block BOTH transports — no channel, must NOT join"
flush_fw
"$ROOT/relay/relay" -p "$PORT_T1" -v >/tmp/pkafb_relayT1.log 2>&1 & PIDS+=($!)
sleep 1
block_udp "$PORT_T1"; block_tcp "$PORT_T1"
(
  export PKERNEL_RELAY="127.0.0.1:$PORT_T1"
  PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 "$BOOT/p-kernel" </dev/null \
      >/tmp/pkafb_T1_n2.log 2>&1 & echo $! >/tmp/pkafb_T1_n2.pid
  sleep 2
  boot_node 1 /tmp/pkafb_T1_n1.log
)
kill "$(cat /tmp/pkafb_T1_n2.pid 2>/dev/null)" 2>/dev/null
sleep 1
flush_fw

T1_regs=$(grep -c "registered" /tmp/pkafb_relayT1.log)
T1_n1_alive=$(grep -c "alive=2" /tmp/pkafb_T1_n1.log)
T1_n2_alive=$(grep -c "alive=2" /tmp/pkafb_T1_n2.log)
echo "    relay registrations: $T1_regs (expect 0)   node1 alive=2: $T1_n1_alive  node2 alive=2: $T1_n2_alive (expect 0/0)"

T1_PASS=0
[ "$T1_regs" -eq 0 ] && [ "$T1_n1_alive" -eq 0 ] && [ "$T1_n2_alive" -eq 0 ] && T1_PASS=1

# =====================================================================
# TEETH 2  (UDP blocked + PKERNEL_RELAY_AUTOFALLBACK=0 -> force UDP-only)
# =====================================================================
PORT_T2=7473
echo
echo "[TEETH 2] UDP blocked + AUTOFALLBACK=0 (force UDP-only) — must NOT join"
flush_fw
"$ROOT/relay/relay" -p "$PORT_T2" -v >/tmp/pkafb_relayT2.log 2>&1 & PIDS+=($!)
sleep 1
block_udp "$PORT_T2"
(
  export PKERNEL_RELAY="127.0.0.1:$PORT_T2"
  export PKERNEL_RELAY_AUTOFALLBACK=0
  PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 "$BOOT/p-kernel" </dev/null \
      >/tmp/pkafb_T2_n2.log 2>&1 & echo $! >/tmp/pkafb_T2_n2.pid
  sleep 2
  boot_node 1 /tmp/pkafb_T2_n1.log
)
kill "$(cat /tmp/pkafb_T2_n2.pid 2>/dev/null)" 2>/dev/null
sleep 1
flush_fw

T2_tcp=$(grep -c "(tcp)" /tmp/pkafb_relayT2.log)
T2_n1_alive=$(grep -c "alive=2" /tmp/pkafb_T2_n1.log)
T2_n2_alive=$(grep -c "alive=2" /tmp/pkafb_T2_n2.log)
echo "    relay (tcp) registrations: $T2_tcp (expect 0)   node1 alive=2: $T2_n1_alive  node2 alive=2: $T2_n2_alive (expect 0/0)"

T2_PASS=0
[ "$T2_tcp" -eq 0 ] && [ "$T2_n1_alive" -eq 0 ] && [ "$T2_n2_alive" -eq 0 ] && T2_PASS=1

# =====================================================================
echo
echo "===================================================================="
echo "  A (auto-fallback to relay-tcp)   : $([ $A_PASS  = 1 ] && echo PASS || echo FAIL)"
echo "  TEETH 1 (both blocked -> no join) : $([ $T1_PASS = 1 ] && echo PASS || echo FAIL)"
echo "  TEETH 2 (AUTOFALLBACK=0 -> none)  : $([ $T2_PASS = 1 ] && echo PASS || echo FAIL)"
echo "===================================================================="
if [ $A_PASS = 1 ] && [ $T1_PASS = 1 ] && [ $T2_PASS = 1 ]; then
    echo "[autofallback-live] PASS"
    exit 0
fi
echo "[autofallback-live] FAIL"
exit 1
