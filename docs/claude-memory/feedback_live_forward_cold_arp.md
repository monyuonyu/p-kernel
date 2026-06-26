---
name: feedback-live-forward-cold-arp
description: "Debugging heuristic for p-kernel [live] forwarding/unicast over the relay: when a multi-process forward 'doesn't deliver' but the routing DECISION and counters look right, suspect the cold-ARP-miss + no-retransmit transport trap, not the routing logic."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

When a p-kernel `[live]` forward over `./relay` (N-2c supernode forward, and any
overlay-unicast path) "doesn't deliver" on the real host while the election /
route DECISION and the local counters look correct, the bug is almost always the
TRANSPORT, not the routing logic. The recurring trap (root-caused on the real
ThinkPad host 2026-06-23, N-2c [live]):

1. The relay overlay is a **BROADCAST medium** — `net_relay_send` (arch/linux/*/net_relay.c)
   only ever emits `REL_BROADCAST` (dst=0). There are NO directed frames on the wire.
2. `udp_send()` **drops on a first-time ARP miss** (netstack.c — "udp_send() drops on
   ARP miss (best-effort)"). ARP is learned only from broadcast traffic. Since SWIM/
   gossip is all broadcast, a node has **never unicast to a specific peer before**, so
   its FIRST unicast (e.g. A→S, or S→B) hits a cold ARP and is **silently dropped**.
3. If the sender does a **single fire-and-forget** `udp_send` with no retransmit, that
   first packet is lost forever — the forward never arrives, even though A logged
   "sent" and via_super_cnt=1.

**Why it's deceptive:** waiting longer does NOT fix it (membership converges, but
nothing populates the unicast ARP until you actually unicast — chicken-and-egg).
PRESEND 11s→30s did not help. The DECISION counters look perfect.

**The cure (the proven pattern):** mirror `arch/common/ss6_live.c`, which survives the
identical race via **retransmit** — `SL_RETRIES 3 × SL_WAIT_MS 200` keyed to an
end-to-end ACK; the retries ride through once ARP resolves; timeout → fail-closed to
DIRECT. N-2c's `snf_send` originally lacked this (its own comment admitted "a full
ACK/retry is a deferred hardening") — adding it (A retransmits SNF_FWD until the
end-to-end SNF_ACK B→S→A, exactly-once via (origin,seq) dedup) made N-2c [live] PASS.

**Second trap, same saga:** a real hosted node's console is FLOODED with `[moe] peer
score` spam (the living mind's DMN/moe firing). A `[live]` verdict that depends on a
post-hoc interactive shell command (e.g. `snf recv`) gets STARVED — the command never
runs, verdict capture is empty (looks like "no delivery"). Cure: have the destination
**self-report at delivery time** (print the verdict from the sink, with a REAL byte
compare) so observation doesn't depend on a starved interactive shell.

**Method note that paid off:** the sandbox is aarch64 PRoot — it CANNOT run x86_64
natively and KILLS backgrounded multi-process harnesses, so the in-proc cert (socket
stubbed) structurally cannot catch this. Only the real-host run (tar-over-ssh →
native build → run) finds it. The commander (with the SSH host) must localize [live]
transport bugs empirically; the implementer (sandbox-only) fixes from that ground truth.

Related: [[feedback_aarch64_irq_path_pitfall]] (suspect the vector before the device),
[[feedback_development_method_is_the_life]] (commander localizes, implementer fixes,
separate auditor verifies).
