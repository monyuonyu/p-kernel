---
name: moment-2026-05-26-phase-b-substep2
description: "2026-05-26 (later) — Phase B sub-step 2 done: kernel-side net_relay.c on aarch64 + x86_64. Two ./p-kernel processes mesh through ./relay over v2 wire end-to-end (REGISTER → BROADCAST gossip → forwards), runtime selection via PKERNEL_RELAY_HOST. Commit 7cb290e."
metadata: 
  node_type: memory
  type: project
  originSessionId: a4128c94-5332-49d8-ac74-41541259901a
---

# 2026-05-26 (later) — Phase B sub-step 2 (client-side relay transport)

The Phase B v2 relay ([[moment-2026-05-26-phase-b-v2]]) had no
in-kernel consumer until now — only relay/test_relay.c exercised the
wire. This commit makes p-kernel itself able to use the relay as its
transport instead of loopback UDP. With this, the Android fleet
(Phase C) has a credible substrate beneath it.

## What landed (commit 7cb290e)

- `arch/linux/{aarch64,x86_64}/net_relay.c` — client side of the v2
  wire. REGISTER on init, opportunistic KEEPALIVE every 25 s from
  send/recv hot paths (no extra thread), wraps each outbound
  Ethernet frame as `RelayPacket{BROADCAST}`. Initial nonce =
  `(wall_clock_seconds << 24) | 1` so a client restart always
  exceeds the relay's stored max_nonce.
- `arch/linux/{aarch64,x86_64}/net_dispatch.c` — owns the public
  `arch_linux_net_{init,send,recv,node_id}` symbols and chooses
  backend at init() time based on `PKERNEL_RELAY_HOST`.
- `arch/linux/{aarch64,x86_64}/net_unix.c` — exported symbols renamed
  `arch_linux_net_*` → `net_unix_*` so the dispatcher can own the
  public names.
- `boot/linux{,_x86_64}/Makefile` — pull `relay/sha256.c` into the
  kernel build via a new `RELAY_DIR / RELAY_OBJS` section and add
  `-I$(ROOT)/relay` so `net_relay.c` can `#include "sha256.h"`.
- `relay/sha256.{c,h}` — rewritten using plain `unsigned int` /
  `unsigned long long` / `unsigned char` instead of stdint typedefs.
  See "decisions" below.

## Runtime selection

```
PKERNEL_RELAY_HOST unset           → net_unix (loopback, unchanged)
PKERNEL_RELAY_HOST set + KEY set   → net_relay v2 (HMAC-SHA256)
PKERNEL_RELAY_HOST set, KEY unset  → net_relay v1 (insecure mode)
```

Both ./p-kernel boots still pass smoke with no env vars.

## End-to-end evidence

`./relay -p 27450` with a 32-byte key + two ./p-kernel processes
(NODE_ID=1, NODE_ID=2, PKERNEL_AUTONET=1) successfully:

- REGISTER both nodes via v2 (relay accepted the HMACs)
- Stream BROADCAST gossip in both directions (SWIM + pmesh + ARP
  visible in relay -v log as `rx v2 type=4` lines)
- Each node prints `[net] transport = relay (node N)` at boot.

## Decisions worth remembering

- **sha256.h uses plain types, not stdint.** The T-Kernel `include/`
  ships its own `stdint.h` shadow with `uint64_t = long long`. The
  system POSIX `bits/stdint-intn.h` defines it as `long`. Both 8 B on
  LP64 but they're typedef-incompatible. The relay/sha256.h originally
  used `<stdint.h>`; when net_relay.c included it alongside POSIX
  sockets, the compiler choked. Fix: sha256.h declares its own
  unsigned types. The wire is unaffected. RFC 4231 KATs still pass.
  This is the third LP64/typedef trap recorded in this project; the
  other two are [[feedback-lp64-typedef-trap]] and
  [[feedback-lp64-allocator-trap]].
- **Sha256 lives in `relay/`, kernel build pulls from there.** The
  alternative was moving it to `arch/common/` or `lib/crypto/`.
  Chose minimal scope: relay is the canonical owner since it wrote
  the lib; one client (the kernel build) reaches in. If a third
  consumer appears, promote to `arch/common/`.
- **BROADCAST, not DATA.** The existing `arch_linux_net_send(frame,
  len)` API doesn't carry a destination node_id — net_unix just
  sendto's every peer in 1..MAX_NODES. To preserve that semantic
  with no caller changes, net_relay.c wraps each frame as
  `RelayPacket{type=BROADCAST}` and lets the relay fan out. v3 could
  add an API extension for explicit dst.
- **Existing net_unix.c duplication preserved.** aarch64 and x86_64
  copies of net_unix.c are identical, and now net_relay.c +
  net_dispatch.c are duplicated similarly. [[feedback-arch-common-
  layout]] says they should be in `arch/common/`; the dedupe is its
  own commit, separate from the Phase B work. Noted in the commit
  message.
- **KEEPALIVE is opportunistic, not threaded.** Triggered from both
  net_relay_send and net_relay_recv if it's been >25 s since the
  last outbound packet. The kernel polls recv frequently enough that
  even an idle node stays alive in the relay's table.

## Cross-links

- [[moment-2026-05-26-phase-b-v2]] — the relay-side v2 work this
  builds on.
- [[project-ump-android-node]] — Phase C now has a real transport to
  plug into.
- [[feedback-lp64-typedef-trap]] — third recurrence of the LP64
  typedef pattern.
- [[feedback-arch-common-layout]] — followup dedupe target.
