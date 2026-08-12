---
name: feedback_proot_sandbox_net_limits
description: The aarch64 PRoot dev sandbox cannot reproduce low-rmem_max / CAP_NET_ADMIN CI conditions; verify those on the ThinkPad runner.
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

When recovering a CI failure that depends on a host network/kernel knob, the aarch64 PRoot-Distro dev sandbox CANNOT faithfully reproduce several conditions — confirmed 2026-06-28 while fixing the Path W SO_RCVBUFFORCE CI failure:

- `net.core.rmem_max` / `wmem_max` are **unreadable and unwritable** even as uid 0: `/proc/sys/net/core/*` returns "Permission denied" (read-only + hidepid); there is no `sudo` binary; `sysctl -w` fails.
- A network namespace is **unavailable**: `unshare -rn` fails with "Invalid argument" (PRoot blocks CLONE_NEWNET/NEWUSER), so you cannot get a fresh-netns default rmem_max=212992 either.
- The sandbox does **not** grant real `CAP_NET_ADMIN`: `SO_RCVBUFFORCE`/`SO_SNDBUFFORCE` return EPERM here (uid is 0 but the cap is not held), so even a contrived low-rmem case falls back to plain `SO_RCVBUF`.
- The sandbox's effective `rmem_max` is ~16MB, so plain `SO_RCVBUF` caps a 16MB request at 33554432 (=2×16MB, the kernel doubles). That high cap is exactly why rmem-sensitive samples (e.g. 42_one_mind) PASS here but FAIL on the ThinkPad runner (rmem_max=212992 → plain caps at ~425984).

**Why:** a team-lead "reproduce the ThinkPad by `sudo sysctl -w rmem_max=...`" plan can be physically impossible in this sandbox; don't fake a pass, and don't burn time hunting for a workaround that isn't there.

**How to apply:** prove the *mechanism* instead — a tiny standalone probe (request a buffer ABOVE the current rmem_max; show plain `SO_RCVBUF` caps at 2×rmem_max while `SO_RCVBUFFORCE` bypasses, or EPERMs here), plus the relay binary's own getsockopt readback (control vs treatment are IDENTICAL on this sandbox = it cannot discriminate, which itself root-causes the sandbox-pass/ThinkPad-fail split). State the predicted ThinkPad numbers (control 2×212992=425984 capped vs treatment full 16MB) and flag that the real discrimination must be read off the ThinkPad runner's relay.log. Getsockopt always returns the DOUBLED value — say so. Also: NEVER `pkill -f 'p-kernel'` — the worktree path contains "p-kernel" so it kills concurrent agents' builds/runs (and the run.sh itself); kill by explicit PID or let the sample's own cleanup trap do it. See [[feedback_hosted_relay_stack_overflow]], [[feedback_live_forward_cold_arp]].
