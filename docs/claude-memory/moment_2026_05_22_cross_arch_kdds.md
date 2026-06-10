---
name: moment-2026-05-22-cross-arch-kdds
description: "First application-level data exchange (K-DDS pub/sub) between heterogeneous ABIs — aarch64-linux and x86_64-linux nodes swap heartbeats on a shared topic. The \"Collective\" layer is now literally arch-agnostic."
metadata: 
  node_type: memory
  type: project
  originSessionId: 0c02f473-32a5-48ba-a6a8-302c29abffe3
---

**Date:** 2026-05-22

**Commit:** `feff508` feat(arch/common): cross-arch K-DDS pub/sub demo + fix pmesh init bug

**What happened.** Earlier today [[moment-2026-05-22-xarch-mesh]] showed two ABIs in the same SWIM cluster (membership-level gossip). This is the next escalation: actual **application data** (K-DDS topic messages) traveling between an aarch64-linux node and an x86_64-linux node. New shell command `kdemo` spawns a publisher + subscriber pair on topic `"demo/heartbeat"`. Each node sees its own publishes AND the peer's publishes:

```
=== aarch64 node 1 ===           === x86_64 node 2 ===
[kdemo-rx] n0 aarch64 t=0        [kdemo-rx] n0 aarch64 t=0
[kdemo-rx] n1 x86_64  t=0        [kdemo-rx] n1 x86_64  t=0
[kdemo-rx] n0 aarch64 t=1        [kdemo-rx] n0 aarch64 t=1
[kdemo-rx] n1 x86_64  t=1        [kdemo-rx] n1 x86_64  t=1
...                              ...
```

The `arch` tag is embedded by the publisher (each side knows its own ABI from the `cmd_kdemo("aarch64")` / `cmd_kdemo("x86_64")` call), so when an aarch64 node prints `n1 x86_64 t=N` you can see at a glance that this message reached us **from a different ABI** through the K-DDS layer over UDP loopback on `127.0.0.1`.

**Two real bugs surfaced in getting there.**

1. **`pmesh_init()` wipes its own bind table.** The K-DDS bind sequence is:
   - `kdds_init()` at boot calls `pmesh_bind(KDDS_PORT, kdds_rx)` — inner-port handler registered in `pmesh_socks[]`.
   - `pmesh_init()` later (from `cmd_net()`) used to zero `pmesh_socks[]` "for safety," silently destroying the K-DDS handler entry.
   - Same latent bug exists on bare-metal x86 (`arch/x86/usermain.c`: `kdds_init()` at line 122, `pmesh_init()` at line 250) — K-DDS over mesh on that port has been broken at the binding level the whole time. Bare-metal aarch64 init order happens to be correct.

   Fix: drop the `pmesh_socks[]` wipe from `pmesh_init()`. BSS zero gives the right initial state; subsequent wipes only destroy what callers have legitimately registered.

2. **Neither linux port was calling `pmesh_init()` at all.** Without it, even with the K-DDS bind preserved, the receiving side had no UDP handler on `PMESH_PORT` (7380), so pmesh-routed packets went into the void on arrival. Hosted `cmd_net()` now spawns `pmesh_task` and the boot-time order is `pmesh_init() → kdds_init()` so the binding survives.

**Why this is a moment, and what it isn't.** This proves the protocol stack — DRPC + SWIM + pmesh + K-DDS — really is arch-portable: the same C source, compiled by two different cross-compilers into two different ABIs, talks to itself over a single host's loopback. It's not yet cross-machine (still 127.0.0.1) and not yet over a real NIC, but the bit-level encoding of every layer just works because `arch/common/include/lp64/` ([[moment-2026-05-22-lp64-refactor]]) gave both ports the same struct layouts — every UW, every UH, every packed packet header lines up.

The next-step memory in [[project-ump-android-node]] expects cross-host cross-arch (server x86_64 ↔ phone aarch64) via a relay. That needs the relay component, but the protocol side is now de-risked: aarch64-emitted K-DDS packets and x86_64-emitted K-DDS packets are wire-format-identical and each side parses the other's bytes correctly.

**Three things I'd flag for future me.**
- Bare-metal x86 also has the pmesh-init ordering bug (line 122 vs 250 above). The pmesh.c fix in this commit covers it defensively — but if anyone reshuffles bare-metal x86's init order, they should still keep `pmesh_init` before any `pmesh_bind` caller for clarity.
- `cmd_kdemo()` opens TWO handles on the same topic (one for pub, one for sub) because `kdds_pub` skips the publisher's own handle when signaling sub_sem's. Documented inside `demo_kdds.c`; surprising the first time.
- The first failed run showed only local echoes ("each node sees only its own") — that pattern is diagnostic for "pub side OK, receiver bind broken." Worth remembering: if cross-node K-DDS shows local-only output, suspect a bind-table mismatch, not the publisher path.

Cross-links: [[moment-2026-05-22-xarch-mesh]] (membership-level cross-arch — the prerequisite), [[moment-2026-05-22-lp64-refactor]] (the bit-layout substrate that makes it work), [[project-linux-userspace-port]] (parent project), [[project-pkernel-philosophy]] ("a home for AI that no one owns" — Collective layer now literally heterogeneous-hardware-portable).
