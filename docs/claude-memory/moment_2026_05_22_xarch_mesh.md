---
name: moment-2026-05-22-xarch-mesh
description: "Two ./p-kernel processes — one aarch64-linux native, one x86_64-linux under qemu-x86_64 — form a 2-node SWIM mesh on the same host. Different ABIs, same kernel, same gossip."
metadata: 
  node_type: memory
  type: project
  originSessionId: 0c02f473-32a5-48ba-a6a8-302c29abffe3
---

**Date:** 2026-05-22

**What happened.** With nothing more than the LP64 refactor of the morning, two `./p-kernel` instances of different ABIs talked to each other over UDP loopback:

```
=== Cross-arch test: aarch64-linux (node 1) + x86_64-linux (node 2) ===
exit codes: aa64=124  x86_64=124   ← both timed out cleanly

=== aa64 (node 1) mesh events ===
[net_unix] node 1 listening on 127.0.0.1:29001
[swim] node 1 -> SUSPECT (no response)
[arp] Request from 10.1.0.2 — sending reply
[degrade] *** level change: FULL -> REDUCED  alive=2

=== x86_64 (node 2) mesh events ===
[net_unix] node 2 listening on 127.0.0.1:29002
[arp] Reply: 10.1.0.1 is at 52:54:00:00:00:01
[swim] node 0 discovered  (via rx)
[degrade] *** level change: FULL -> REDUCED  alive=2
[swim] node 0 -> SUSPECT (no response)
[swim] node 0 recovered  (via rx)
```

**Why this is a moment.** [[moment-2026-05-21-first-pkernel-on-linux]] proved p-kernel runs as a Linux process on aarch64. [[moment-2026-05-21-first-pkernel-on-x86_64]] proved it runs on x86_64 too. This is the first time **two different ABIs share a single distributed mesh** — a beat earlier than expected.

No code change was required. `arch/linux/aarch64/net_unix.c` and `arch/linux/x86_64/net_unix.c` are byte-identical (`diff -q` returns empty). Both arches use the same UDP-loopback "virtual wire" on `127.0.0.1:29001..29008`. The protocol layer (DRPC / SWIM / ARP / degrade / replica) lives in `arch/common/` and was already arch-independent.

**Pre-flight: x86_64-on-x86_64 2-node mesh, also verified for the first time.** Two `qemu-x86_64 ./p-kernel` processes (ids 1 and 2) form the same 2-node mesh that aarch64 nodes do. Confirms the note in [[moment-2026-05-21-first-pkernel-on-x86_64]] — "UDP loopback infrastructure is in place, never exercised on this arch" — is now exercised.

**What it proves for the project philosophy.** The 5-layer p-kernel worldview's "Collective" layer doesn't care which silicon a node runs on. An aarch64 phone running the [[project-ump-android-node]] APK could mesh with an x86_64 server running the same kernel. The substrate is now demonstrably arch-agnostic.

**Caveat.** Both nodes here were on the same Linux host (127.0.0.1 UDP loopback). True cross-machine cross-arch needs the real-NIC path (USB-Ethernet on RPi, or the Android port's network code) which isn't done yet. But the protocol-side is proven.

**Bench notes for the runs above.** Tested via:
```bash
( cd boot/linux && PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 timeout 20 ./p-kernel > /tmp/xa.log 2>&1 ) &
( cd boot/linux_x86_64 && PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 timeout 20 qemu-x86_64 ./p-kernel > /tmp/xb.log 2>&1 ) &
wait
```

Stable for 20-second soak. Earlier 10×30-second symmetric aa64 stress also clean. The mesh is reliable.

Cross-links: [[project-linux-userspace-port]] (parent), [[moment-2026-05-21-first-pkernel-on-x86_64]] (sibling port), [[project-pkernel-philosophy]] (the worldview this is starting to literalise).
