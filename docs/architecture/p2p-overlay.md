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

### N-2 slice 1 — the selection function + host cert (DONE, wave-n2-supernode-select)
The **deterministic selector only** is implemented, exactly mirroring `region_coordinator()`:
- `arch/common/region.c` / `region.h`: `region_supernode()` recomputes membership (same source of
  truth — `region_recompute()`'s `member[]`), then returns the **LOWEST node id that is BOTH a
  current region member AND supernode-capable**; returns `0xFF` when no capable member exists
  (== "fall back to the central relay", the correct Skype-style degrade). Pure, allocation-free,
  O(N), **integer-only (reads no float) → every arch / every node computes the IDENTICAL id**, so
  the role converges with NO vote/election and survives death by recomputation.
- Capability is a **LOCAL** per-node table `super_capable[DNODE_MAX]` with setter
  `region_set_super_capable()` + self opt-in `PKERNEL_SUPERNODE=1` (read once on hosted nodes,
  default NOT capable — conservative). In slice 1 it was LOCAL only; **slice 2b (below) now
  gossips this bit over SWIM** so the table reflects a converged fleet view.
- Host cert `region_supernode_test()` (shell `region test`, wired in both
  `arch/linux/{aarch64,x86_64}/usermain.c`): 8/8 PASS — lowest-capable-wins (lowest *member* if
  incapable is skipped), convergence/determinism (same id over one synthetic view), survives-death
  (kill the current supernode in the view → next-lowest capable, no election call), relay-fallback
  (0 capable → `0xFF`), non-member-capable ignored, setter bounds-check.

### N-2 slice 2b — SWIM capability-bit gossip (DONE, wave-n2b-capability-gossip)
Makes slice-1's `super_capable[]` table **fleet-real**: each node's self-declared capability now
propagates across the mesh via SWIM, so every node converges on the same supernode with NO vote.
- **Wire (`arch/common/include/swim.h`):** the reserved zero `_pad` byte of `SWIM_GOSSIP_EVT` is
  reused as `UB capability`. The on-wire layout/size is **byte-identical** (entry = 4B, packet =
  24B — static-checked), so **`SWIM_VERSION` is NOT bumped**. Bumping it would make a v1 node
  (`swim_rx` gates on `version != SWIM_VERSION`) DROP the whole packet, losing membership/gossip
  interop, for a strictly-additive zero-default field. **Backward-compat:** an old node emits
  `capability=0` → read as non-capable → relay fallback (safe degrade); a new node ignores the
  field on old peers and never crashes.
- **Self-authoritative origination (`arch/common/swim.c`):** capability is meaningful **only** in a
  node's OWN gossip about itself (`cap_self()` = `region_is_super_capable(drpc_my_node)`, sourced
  from `PKERNEL_SUPERNODE=1`). It rides the node's per-round self-ALIVE beacon gossip
  (`swim_task`, swim.c:~525) and its self-suspicion refutation (swim.c:~197). Every other node
  **relays the byte VERBATIM** (epidemic) — no third party originates a peer's capability.
- **Apply under the SAME LWW gate (`gossip_apply`):** when a peer entry supersedes per the existing
  `(incarnation,state)` last-writer-wins rule (swim.c:~258), it ALSO calls
  `region_set_super_capable(nid, entry.capability)` — done **before** the state-same short-circuit
  so a capability flip carried by a fresh incarnation lands. A **stale lower-incarnation** rumor is
  rejected by the same gate, so capability **cannot regress** on a stale rumor and converges in
  lock-step with membership state. Transitive-discovery (UNKNOWN→adopt) sets/re-propagates the byte
  verbatim too.
- **No change to the selection math** — `region_supernode()`/`supernode_select()` already read
  `super_capable[]`; this slice just makes that table reflect a real converged fleet view.
  Integer-only / deterministic / no-VLA, same on every arch.
- **Host cert `swim_cap_gossip_self_test()` (shell `nodes cap`, both linux usermains):** drives the
  REAL `gossip_apply`. **[cap-gossip-converge]** a fresh `capability=1` self-rumor about X →
  `region_is_super_capable(X)==TRUE` AND `region_supernode()` selects X; two capable peers (X<Y) →
  all views converge on X with no vote (NOCENTRAL). **[cap-gossip-staleness]** a stale
  lower-incarnation rumor that would flip X off is IGNORED; a fresh higher one DOES update it.
  **[cap-gossip-falsifiable]** a `capability=0` self-rumor leaves the node non-capable, and the
  cert FAILS if the apply ignores the byte (verified by a sabotage rebuild). PASS on aarch64-linux
  AND x86_64-linux; `nodes test` (swim-incarn) and `region test` (8/8) regress clean.
- **Honest bound:** in this slice a node's capability is **env-fixed at init** (`PKERNEL_SUPERNODE`),
  so it never changes at runtime. A *runtime* capability flip would need an incarnation bump to
  supersede (same mechanism as the self-suspicion refutation) — deferred, out of scope here. A
  true multi-process LIVE mesh converging the bit over UDP is left as a deferred `[live]` row (the
  in-process cert drives the identical real `gossip_apply` code path).

### N-2 slice 2c — supernode packet FORWARDING (DONE, wave-n2c-supernode-forward)
The elected supernode now actually FORWARDS region traffic — the first datagram routed
THROUGH a supernode instead of unconditionally via the central `./relay`.
- **Module (`arch/common/supernode.c`/`.h`, hosted-only, check_parity allowlisted like
  `ss6_live.c`):** when a node sends a payload to region peer B and `region_supernode()`
  elects a capable peer S (S≠0xFF, S≠self, S≠B, S not SWIM-DEAD), the sender wraps it as
  an `SNF_FWD` to S on a dedicated UDP port (`SNF_PORT`=7377 — distinct from every other
  `udp_bind` port; it was 7380==`PMESH_PORT`, which silently stole all SNF traffic on the
  live forward path until N-2c-live-fix), S re-forwards it as an
  `SNF_DELIVER` to B over the SAME `net_relay` transport (the supernode is just another
  node — NO new wire protocol; `udp_send`/`udp_bind`, mirroring `ss6_live.c`), and B
  receives the payload BYTE-IDENTICAL. The route decision is a PURE integer function
  `snf_route_target(dst,me,sn)` (testable, arch-uniform).
- **Fail-closed / honest degrade:** no supernode (`0xFF`), S==self/dst, OR an elected S
  that SWIM has marked DEAD → DIRECT `SNF_DELIVER` to B (== today's central-relay
  behavior). So a default single node (no capable supernode) is byte-unchanged.
  HONEST bound: a silently-unreachable-but-still-ALIVE S within the SWIM death-detect
  window is a brief loss window (no per-packet ACK/retry — a deferred hardening, the same
  bound `ss6_live.c` documents).
- **NOCENTRAL/deterministic** (S = the existing min-id `region_supernode()` selector, no
  vote); the forwarded payload is **byte-identical** (one mind); no VLA (fixed
  `SNF_PAYLOAD_MAX`, all file-static off the task stack); `region.c`/`swim.c`/mind-math
  UNTOUCHED.
- **Host cert `supernode_forward_self_test()` (shell `region fwd`): 20/20 PASS on BOTH
  linux + linux_x86_64 (cross-arch identical), sabotage-tested RED.** `[supernode-forward]`
  A→B THROUGH elected S (`S.forwarded_count`>0, B byte-identical, origin A preserved) +
  falsifier (a) no supernode → DIRECT (S forwards 0, B still gets it) + falsifier (b)
  unreachable S → fail-closed DIRECT (no packet lost) + a **REAL-production-code-path**
  sub-arm that drives the SHIPPED `snf_rx`/`snf_forward`/deliver functions + counters
  in-process (end-to-end byte-identity through the real `snf_forward`, corruption caught).
- **DEFERRED [live] row:** the true 3-OS-process supernode-forward over `./relay`
  (`samples/11_distributed/run_supernode_fwd.sh`, ready) — not cashed in the implementer's
  PRoot sandbox (foreground `sleep` + backgrounded `relay &`/`p-kernel &` children are
  killed there, the same wall the existing `run_ss6_live.sh`/`run_4node_regions.sh` `[live]`
  rows hit); cash on a real host (the SS-6 → SS-6-live pattern).

**DEFERRED to later N-2/N-3/N-4 slices (NOT in this slice — honest scope):**
- **NAT hole-punch (N-3)** — supernode-assisted STUN / rendezvous / promote-to-P2P-direct.
- **Bootstrap/seed (N-4)** — `PKERNEL_SEED` list; relay as one optional well-known seed.
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
