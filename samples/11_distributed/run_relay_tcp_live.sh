#!/bin/bash
# ---------------------------------------------------------------------------
# connect-anywhere SLICE 3 — LIVE TCP-relay join cert (TCP-LIVE).
#
# Proves a REAL booted ./p-kernel node joins a REAL ./relay over a plain-TCP
# stream (PKERNEL_RELAY_TCP=1, net_relay_tcp.c) and meshes with a peer — the
# kernel-side TCP backend was COMPILE-VERIFIED ONLY before this harness; here
# it actually runs end-to-end.
#
# The relay listens UDP+TCP on the SAME port (relay/relay.c). Two ./p-kernel
# nodes boot with PKERNEL_RELAY_TCP=1 so they speak the TCP backend, register
# over a TCP connection, discover each other via SWIM, and reach membership
# size 2 (the [degrade] alive=2 signal — the SIMPLEST existing assertable
# distributed signal, identical to what run_2node_reduced.sh relies on for
# REDUCED).
#
# Three sub-certs, all load-bearing:
#   A. TCP JOIN     both nodes REGISTER over TCP ("node N registered ... (tcp)"
#                   in the relay -v log, NOT "bad frame len"), and both reach
#                   alive=2 (SWIM membership over the TCP transport).
#   B. EQUIVALENCE  the SAME 2-node scenario over the DEFAULT UDP relay reaches
#                   the SAME observable (alive=2 on both) — one mind, one mesh,
#                   regardless of transport.
#   C. FALSIFIER    with PKERNEL_RELAY_TCP=1 but only a dummy UDP socket bound
#                   on the port (no TCP listener), the node FAILS to join (no
#                   alive=2, no discovery) — proving the TCP path is genuinely
#                   load-bearing, not silently meshing some other way.
#
# Single-host loopback; no external network needed.
#
# Usage:   ./run_relay_tcp_live.sh
# Watch:   /tmp/pktcp_*.log
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"          # .../p-kernel

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "unsupported host arch $(uname -m)"; exit 1 ;;
esac

[ -x "$BOOT/p-kernel" ]    || make -C "$BOOT"        >/dev/null || exit 1
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay"  >/dev/null || exit 1

KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
export PKERNEL_RELAY_KEY="$KEY"

PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

# Drive one node to alive (AUTONET brings up SWIM at boot); let membership
# settle, then quit. $1=node-id $2=logfile  (env already exported by caller).
boot_node() {
    local id="$1" log="$2"
    { sleep 9; echo nodes; sleep 2; echo exit; } \
        | PKERNEL_NODE_ID="$id" PKERNEL_AUTONET=1 "$BOOT/p-kernel" \
        >"$log" 2>&1
}

# =====================================================================
# A. TCP JOIN  (PKERNEL_RELAY_TCP=1, real relay on TCP+UDP)
# =====================================================================
PORT_A=7461
echo "[A] TCP join — relay on :$PORT_A, two nodes with PKERNEL_RELAY_TCP=1"
"$ROOT/relay/relay" -p "$PORT_A" -v >/tmp/pktcp_relayA.log 2>&1 & PIDS+=($!)
sleep 1
(
  export PKERNEL_RELAY="127.0.0.1:$PORT_A"
  export PKERNEL_RELAY_TCP=1
  PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 "$BOOT/p-kernel" </dev/null \
      >/tmp/pktcp_A_n2.log 2>&1 & echo $! >/tmp/pktcp_A_n2.pid
  sleep 2
  boot_node 1 /tmp/pktcp_A_n1.log
)
kill "$(cat /tmp/pktcp_A_n2.pid 2>/dev/null)" 2>/dev/null
sleep 1

A_tcp_regs=$(grep -c "registered.*(tcp)" /tmp/pktcp_relayA.log)
A_badframe=$(grep -c "bad frame"          /tmp/pktcp_relayA.log)
A_n1_alive=$(grep -c "alive=2"            /tmp/pktcp_A_n1.log)
A_n2_alive=$(grep -c "alive=2"            /tmp/pktcp_A_n2.log)
A_n1_xport=$(grep -c "transport = relay-tcp" /tmp/pktcp_A_n1.log)
A_n2_xport=$(grep -c "transport = relay-tcp" /tmp/pktcp_A_n2.log)

echo "    relay TCP registrations: $A_tcp_regs (expect >=2)   bad-frame: $A_badframe (expect 0)"
echo "    node1 transport=relay-tcp: $A_n1_xport   node2 transport=relay-tcp: $A_n2_xport"
echo "    node1 alive=2: $A_n1_alive   node2 alive=2: $A_n2_alive"
echo "    --- relay registration lines (proof: over TCP) ---"
grep -E "registered" /tmp/pktcp_relayA.log | sed 's/^/      /'

A_PASS=0
[ "$A_tcp_regs" -ge 2 ] && [ "$A_badframe" -eq 0 ] \
    && [ "$A_n1_xport" -ge 1 ] && [ "$A_n2_xport" -ge 1 ] \
    && [ "$A_n1_alive" -ge 1 ] && [ "$A_n2_alive" -ge 1 ] && A_PASS=1

# =====================================================================
# B. EQUIVALENCE  (same scenario over the DEFAULT UDP relay)
# =====================================================================
PORT_B=7462
echo
echo "[B] UDP equivalence — same scenario, no PKERNEL_RELAY_TCP"
"$ROOT/relay/relay" -p "$PORT_B" -v >/tmp/pktcp_relayB.log 2>&1 & PIDS+=($!)
sleep 1
(
  export PKERNEL_RELAY="127.0.0.1:$PORT_B"
  PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 "$BOOT/p-kernel" </dev/null \
      >/tmp/pktcp_B_n2.log 2>&1 & echo $! >/tmp/pktcp_B_n2.pid
  sleep 2
  boot_node 1 /tmp/pktcp_B_n1.log
)
kill "$(cat /tmp/pktcp_B_n2.pid 2>/dev/null)" 2>/dev/null
sleep 1

B_udp_regs=$(grep -c "registered"  /tmp/pktcp_relayB.log)
B_tcp_regs=$(grep -c "registered.*(tcp)" /tmp/pktcp_relayB.log)
B_n1_alive=$(grep -c "alive=2"      /tmp/pktcp_B_n1.log)
B_n2_alive=$(grep -c "alive=2"      /tmp/pktcp_B_n2.log)
B_n1_xport=$(grep -c "transport = relay\b" /tmp/pktcp_B_n1.log)

echo "    relay registrations: $B_udp_regs  (of which TCP: $B_tcp_regs, expect 0)"
echo "    node1 transport=relay(udp): $B_n1_xport   node1 alive=2: $B_n1_alive   node2 alive=2: $B_n2_alive"

B_PASS=0
[ "$B_tcp_regs" -eq 0 ] && [ "$B_n1_xport" -ge 1 ] \
    && [ "$B_n1_alive" -ge 1 ] && [ "$B_n2_alive" -ge 1 ] && B_PASS=1

# Transport-agnostic equivalence: identical observable (both nodes alive=2)
# under TCP (A) and UDP (B).
EQUIV=0
[ "$A_n1_alive" = "$B_n1_alive" ] && [ "$A_n2_alive" = "$B_n2_alive" ] \
    && [ "$A_n1_alive" -ge 1 ] && EQUIV=1
echo "    equivalence (A.alive==B.alive, both nodes meshed): $([ $EQUIV = 1 ] && echo YES || echo NO)"

# =====================================================================
# C. FALSIFIER  (TCP mode, only a dummy UDP socket bound — no TCP relay)
# =====================================================================
PORT_C=7463
echo
echo "[C] falsifier — PKERNEL_RELAY_TCP=1 but only a dummy UDP socket on :$PORT_C"
python3 -c "import socket,time;s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.bind(('127.0.0.1',$PORT_C));time.sleep(40)" \
    >/dev/null 2>&1 & PIDS+=($!)
sleep 1
(
  export PKERNEL_RELAY="127.0.0.1:$PORT_C"
  export PKERNEL_RELAY_TCP=1
  PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 "$BOOT/p-kernel" </dev/null \
      >/tmp/pktcp_C_n2.log 2>&1 & echo $! >/tmp/pktcp_C_n2.pid
  sleep 2
  boot_node 1 /tmp/pktcp_C_n1.log
)
kill "$(cat /tmp/pktcp_C_n2.pid 2>/dev/null)" 2>/dev/null
sleep 1

C_connfail=$(grep -cE "connect failed|connect timed out|connect:" /tmp/pktcp_C_n1.log)
C_n1_alive=$(grep -c "alive=2"   /tmp/pktcp_C_n1.log)
C_n1_disc=$(grep -c "discovered" /tmp/pktcp_C_n1.log)
echo "    node1 TCP connect failed: $C_connfail (expect >=1)"
echo "    node1 alive=2: $C_n1_alive (expect 0)   node1 discovered: $C_n1_disc (expect 0)"

C_PASS=0
[ "$C_connfail" -ge 1 ] && [ "$C_n1_alive" -eq 0 ] && [ "$C_n1_disc" -eq 0 ] && C_PASS=1

# =====================================================================
echo
echo "===================================================================="
echo "  A (TCP join over TCP)        : $([ $A_PASS = 1 ] && echo PASS || echo FAIL)"
echo "  B (UDP baseline meshes)      : $([ $B_PASS = 1 ] && echo PASS || echo FAIL)"
echo "  equivalence (A==B observable): $([ $EQUIV  = 1 ] && echo PASS || echo FAIL)"
echo "  C (falsifier fails to join)  : $([ $C_PASS = 1 ] && echo PASS || echo FAIL)"
echo "===================================================================="
if [ $A_PASS = 1 ] && [ $B_PASS = 1 ] && [ $EQUIV = 1 ] && [ $C_PASS = 1 ]; then
    echo "[tcp-live] PASS"
    exit 0
fi
echo "[tcp-live] FAIL"
exit 1
