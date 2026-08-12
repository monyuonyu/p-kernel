---
name: feedback_relay_rmem_host_config
description: "Path W relay buffer fix is TWO-part: SO_RCVBUFFORCE code is a NO-OP in CI because the runner container drops CAP_NET_ADMIN; the real lever is the HOST's net.core.rmem_max (raised to 32MB on the ThinkPad runner)."
metadata:
  node_type: memory
  type: feedback
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

2026-06-28: the Path W relay overflow (84KB rw[] fold burst tail-dropped) needs a
big UDP rcvbuf. MEASURED ground truth on the ThinkPad CI runner:

- `plain SO_RCVBUF` is SILENTLY CAPPED to `net.core.rmem_max`. The ThinkPad host
  defaulted to 212992 (~208KB) → request of 16MB readback = 2×212992 = 425984 → FAIL.
- `SO_RCVBUFFORCE` bypasses rmem_max but needs **CAP_NET_ADMIN**. The CI runner
  container `pkernel_gh_runner` runs `CapAdd=[] Privileged=false NetMode=host`
  (Docker's default drops NET_ADMIN) → FORCE returns **EPERM** → falls through to
  plain → still capped. So the SO_RCVBUFFORCE *code change alone is a NO-OP in CI*.
- THE REAL FIX: raise the **host** `net.core.rmem_max` (the container shares it via
  `--network host`). Set to 33554432 in `/etc/sysctl.d/99-pkernel-relay.conf`
  (+wmem_max) on the runner; verified container plain SO_RCVBUF(16MB) → 33554432.
  This host sysctl is NOT in the git repo — it lives only on the runner + is
  documented in the relay.c comment and commit d614cf50's message.

**Why:** explains the sandbox-pass / ThinkPad-fail split (the gcc sandbox has
rmem_max ~16MB so plain SO_RCVBUF already gets the burst buffer; the ThinkPad
doesn't). Also: the sandbox CANNOT reproduce/verify this (no sudo, /proc read-only,
no CAP_NET_ADMIN → FORCE EPERMs, netns blocked by PRoot) — the faithful proof is a
ThinkPad-runner / real-CI job, never a sandbox one.

**How to apply:** for any relay/socket-buffer throughput work, both pieces are
load-bearing — the FORCE code (wins where the cap IS held) AND the host sysctl
(makes the plain fallback succeed under the cap-less container). When a
buffer/throughput fix "passes in the sandbox but fails on the ThinkPad," suspect a
host sysctl cap (rmem_max/wmem_max) or a missing container capability, not the code.
Related: [[feedback_live_forward_cold_arp]], [[project_survival_network]].
