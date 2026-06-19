# p2p-overlay — the Skype-original-like decentralized network (design + critique)

**Status: design done + adversarial critique VERDICT: SOUND (2026-06-19).** mk_pino: "Skypeの
ようなネットワークを築きたい" — a P2P mesh with NO central server; the relay becomes a role,
not THE network. Honest scope (critique): the cheap "zero-kernel-change / one env flag" claim is
HOST-ONLY; Android + decentralized identity have real prerequisites (below).

## Vision
Every node is a peer, not a client of a server. Reachable peers (same LAN, or both routable)
exchange Ethernet frames over direct UDP, NO intermediary. NAT'd peers are forwarded by an
**emergent supernode** — any capable node, chosen deterministically-from-SWIM-membership (same
pure function as region coordinator / teacher selection; no election, no registrar, Raft stays
unwired; the role survives death by recomputation). Degrades gracefully LAN-direct → P2P-direct
→ supernode-relayed, exactly like Skype's original. Wire stays HMAC-SHA256 v2; membership stays
SWIM gossip.

## N-1 LAN-direct — THE cheapest first slice (solves mk_pino's 2 same-WiFi phones, NO relay)
The transport seam is real: `arch/linux/{x86_64,aarch64}/net_dispatch.c` selects backends behind
a 4-symbol ABI (`net_*_init/send/recv/node_id`); today `net_unix.c` (loopback switch) and
`net_relay.c` (central relay). Add a 3rd: **`arch/linux/common/net_lan.c`**, selected by
`PKERNEL_LAN=1`:
- `socket(AF_INET,SOCK_DGRAM)`, `SO_REUSEADDR`+`SO_BROADCAST`, bind `0.0.0.0:7351`.
- `net_lan_send(frame,len)` → send raw Ethernet frame to `255.255.255.255:7351` (rendezvous) AND
  unicast to every **learned** peer.
- `net_lan_recv()` → recvfrom + **learn the source sockaddr** into a peer table (broadcast is only
  for first contact; thereafter unicast direct).
SWIM's existing once/sec broadcast beacon (the NET-DISCOVERY-STAR limited-broadcast in swim.c)
becomes a real LAN UDP broadcast → phone A boots, broadcasts; B learns A's addr, marks ALIVE; both
populate dnode_table; region forms; K-DDS/mind traffic flows — all direct LAN UDP, no relay
process anywhere. = `net_unix.c` with `127.0.0.1` → LAN-broadcast + a learned-address table.
Caveat: relies on the AP forwarding UDP broadcast (most home routers do; "client isolation" APs →
fall back to 224.0.0.x link-local multicast via the existing `udp_join_group`, or the supernode).

## Supernodes (N-2), NAT traversal (N-3), bootstrap (N-4)
- Supernode = capable (reachable + volunteered) node; per-NAT'd-peer supernode = lowest-id capable
  node in its region, recomputed locally by all → convergence with no vote; relay.c's
  REL_DATA/REL_BROADCAST forwarder lifts in (relocation, not new logic); a capability bit is
  gossiped. Survives death like a region coordinator.
- NAT hole-punching: supernode-assisted STUN (it already holds the peer's reflexive tuple);
  RENDEZVOUS → simultaneous open → promote to P2P-direct. Cone NATs succeed; **both-symmetric stays
  supernode-relayed permanently (correct — the relay IS the fallback, no TURN needed).** CGNAT
  (mobile) is often symmetric → LAN-direct is the high-value win.
- Bootstrap: LAN broadcast first (zero config for same-WiFi); else `PKERNEL_SEED=host[:port],...`
  (shape of today's PKERNEL_RELAY list); the relay = one optional well-known seed. Hard bound: an
  all-NAT'd fleet with no seed and no shared LAN cannot cold-start — ≥1 reachable seed or LAN
  rendezvous is necessary (no MANDATORY central one; every seed is replaceable).

## MUST-FIX (critique, before/within the waves)
1. Scope "zero kernel changes / 120 lines / one env flag" to **HOST ONLY**; Android LAN-direct also
   needs a new TU in the android CMakeLists (run check_parity.sh) + JNI to set PKERNEL_LAN + a
   WifiManager.MulticastLock for broadcast/multicast RX.
2. **Decentralized node-id is a HARD PREREQUISITE of N-1** (= N-0): two phones default to id 1 and
   `swim_rx` ignores each other as self. Pick the key-derived/random id scheme now.
3. State the **PSK/identity boundary**: LAN-direct meshes nodes sharing a PSK (mk_pino's own
   phones), NOT arbitrary strangers; stranger-meshing needs an Ed25519 identity handshake (later).
4. **Malicious-supernode** threat: HMAC protects payload, not routing; specify behavior-driven
   detection/rotation (cross-check delivery via two independently-derived supernodes).
5. Host 2-node dev test: SO_REUSEPORT load-balances datagrams across 2 processes on ONE host — use
   two network namespaces + veth (or two machines), not two processes on one host.

## Cheapest first slice (start here)
Host-only two-node LAN-direct on two namespaces/veth: write net_lan.c (net_unix.c template),
wire into net_dispatch.c behind PKERNEL_LAN=1 (3 externs + one if-branch), set distinct
PKERNEL_NODE_ID per node + same PKERNEL_RELAY_KEY (PSK), cert = two nodes co-region with NO relay.
Then N-0 (real decentralized id) → Android LAN-direct → supernodes/NAT/seed.
