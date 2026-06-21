---
name: feedback_the_debug_env_is_real
description: "mk_pino set up a real SSH host (ThinkPad) for multi-node/[live] verification — USE it; it found a real bug on first use that the sandbox structurally can't."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

2026-06-21: mk_pino asked "前教えたpcのデバッグ環境は使ってる？" — I had NOT been using it
(all verification was local QEMU). That was a miss. It is a REAL, reachable resource:

- **ThinkPad X1 Carbon, x86_64, 4-core**, `~/pkernel` pre-built. `ssh 192.168.10.100 -l shota`,
  pass `ihavecontrol@0840` (sshpass works in the sandbox). **VERIFICATION ONLY; sudo / package
  installs NOT authorized.** The sandbox's own IP (as the ThinkPad sees it) is **192.168.10.56**
  — same /24, so the two are mutually reachable (true 2-machine tests are feasible).
- **No rsync in the sandbox** → ship code with `tar czf - --exclude=.git ... | ssh ... 'tar xzf -
  -C ~/pkernel-verify'` (a SEPARATE dir, non-destructive — never overwrite their ~/pkernel).
  Then build on the ThinkPad + run.

**Why it matters (the payoff, first use):** every distributed cert in this repo carries a
"`[live]` ran 8 processes on ONE host; SSH 2-machine deferred" floor, because the **PRoot sandbox
kills backgrounded long-lived `relay &`/`p-kernel &` children** (so `run_*_live.sh` can't run
here at all). The ThinkPad runs them. On the very first run it FOUND A REAL BUG the `[in-proc]`
cert structurally could not: N-2c supernode-forward delivered `forwarded=0` `[live]` — root cause
a **UDP port collision SNF_PORT==PMESH_PORT==7380** (netstack udp_input dispatches to the FIRST
matching socket; pmesh bound 7380 first → SNF traffic eaten). Fixed → 7377. (The [live] then
revealed MORE layers — membership-routing timing + a harness verdict off-by-mapping — still OPEN;
do NOT fudge green.)

**Rule going forward:** wire the SSH host into the distributed-cert verification — drive each
distributed wave's `[live]` to a real-host PASS (the SS-6→SS-6-live pattern), not just an
`[in-proc]` cert (the 2026-06-14 critique's exact demand). Caveats: 2 procs on one host can't
share a broadcast/REUSEPORT port (needs 2 netns = sudo, NOT authorized, OR 2 real machines);
the x86_64 binary SIGILLs under direct exec in the sandbox but runs under qemu-x86_64. The
in-proc cert that STUBS the socket hop will pass while the live transport is broken — the real
host is the only honest check. See [[feedback_audit_is_the_engine]], [[project_multicore_arc]].
