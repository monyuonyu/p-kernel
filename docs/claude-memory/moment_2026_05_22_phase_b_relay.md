---
name: moment-2026-05-22-phase-b-relay
description: "First piece of Phase B shipped — a 200-line C UDP relay + 2-client round-trip test. The NAT-traversal substrate the UMP phone fleet plan rests on, now real code."
metadata: 
  node_type: memory
  type: project
  originSessionId: 0c02f473-32a5-48ba-a6a8-302c29abffe3
---

**Date:** 2026-05-22

**Commit:** `03edb21` feat(relay): Phase B v1 — public UDP forwarder + design doc

**What happened.** [[project-ump-android-node]] always called Phase B's relay server out as "one tiny public IP" without anyone having drawn the wire protocol or written the code. Today that became a working binary. `relay/relay.c` is ~200 lines of C. It listens on a single UDP port (default 7400), parses a 12-byte `RelayPacket` header, maintains `{node_id → (UDP addr, last_seen)}` for up to 255 nodes, and forwards `DATA` packets by `dst_node`. `relay/test_relay.c` spawns the relay as a child, forks two simulated client processes that REGISTER and exchange DATA packets, and confirms each receives the other's payload — the canonical round-trip:

```
[relay] node 1 registered: 127.0.0.1:60980
[relay] node 2 registered: 127.0.0.1:39795
[relay] rx type=2 src=1 dst=2 ... (24 B)
[relay] rx type=2 src=2 dst=1 ... (24 B)
[client n1] received: 'hello from B' (12 B)
[client n2] received: 'hello from A' (12 B)
[relay-test] PASS — both payloads round-tripped through relay
```

**Design fits an existing seam.** `pmesh` packets already carry `src_node` + `dst_node` in the header. The relay's wire format is just a 12-byte wrapper carrying the same two fields. When the client-side `net_relay.c` lands, it'll wrap pmesh-DATA bytes in a RelayPacket-DATA and send to the relay; the relay unwraps just enough to know who to forward to, and the receiving client unwraps and hands the inner payload to the existing `pmesh_rx`. No protocol layer above pmesh needs to know the relay exists.

**Non-features I deliberately did not write.** Auth, encryption, replay protection, DDoS limiting, multi-relay federation, hole-punching. The threat model in `docs/phase_b_relay.md` is explicit: "two phones I own on a relay I run." HMAC + per-network keys are v2; TLS-like AEAD is v3; STUN-style direct path is "later." Listing the non-features beats letting them creep into v1.

**Pitfall caught and pinned.** `signal()` on glibc Linux defaults to SA_RESTART, so `recvfrom()` auto-restarted on SIGTERM and the main loop never observed `stop`. First test run hung in the parent's `waitpid(relay)`. Fix: `sigaction()` with empty `sa_flags` so the syscall returns EINTR cleanly. Worth remembering for any future POSIX-signal-driven server pattern in this repo.

**Why C, not Go.** Matches the rest of the project — one toolchain, one binary format, no Cargo/go.mod drift. Documented as a deliberate choice; a Go relay is welcome as a parallel implementation if anyone wants one. The 12-byte wire header is the load-bearing piece; any implementation that gets it right interops.

**Where Phase B stands now.**
- ✅ Sub-step 1: relay protocol + server + round-trip test (this commit)
- 🔜 Sub-step 2: `arch/linux/aarch64/net_relay.c` — client-side TU that swaps loopback for relay
- 🔜 Sub-step 3: 2 `./p-kernel` instances on different hosts talking through the relay
- 🔜 Sub-step 4: Android NDK build talks to relay (the actual phone scenario)

The protocol shape is locked, the relay is verified, the doc spells out exactly how the client integration plugs in. Sub-step 2 should be a focused single-TU job next session — `arch_linux_net_init` resolves `PKERNEL_RELAY_HOST`, opens a UDP socket to the relay, sends REGISTER; `arch_linux_net_send` adds the 12-byte header; `arch_linux_net_recv` strips it; periodic keepalive timer for NAT.

**Subtle point worth keeping.** The relay's table is keyed on `node_id` not `(ip, port)`. That means a phone roaming between Wi-Fi and cellular keeps the same node identity — when it next sends a packet, the relay updates its address binding silently. No reconnect dance, no session state. That falls out of the design naturally; flagging because future hole-punching work needs to preserve this property.

Cross-links: [[project-ump-android-node]] (parent strategy), [[moment-2026-05-22-cross-arch-kdds]] (the cross-arch K-DDS that this relay extends across the public internet), [[project-pkernel-philosophy]] ("a home for AI no-one owns" — single-relay v1 violates this in spirit; F-Droid + bring-your-own-relay restores it long-term, which is why the doc names the non-features).
