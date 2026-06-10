---
name: feedback-hosted-relay-stack-overflow
description: "Hosted-port trap: large stack buffers (buf[MAX_PKT]=1416B) in net_relay.c blow the small T-Kernel task stacks (2048-4096) on deep send/recv chains. Symptom: garbage-PC crash (pc==fault addr) only in multi-node networked runs. Fix: make per-packet scratch buffers static."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: ea4edf68-51f5-4387-a8b6-bcec32dd0fe8
---

When a multi-node `./p-kernel` mesh crashes with a **garbage program counter** (SIGBUS or SIGSEGV where `pc == si_addr == a random 64-bit value`), suspect a **task stack overflow corrupting a saved return address** — not a wild pointer. The raw-asm context switch restores a clobbered LR from the victim task's stack and `ret`s into nowhere. `-fstack-protector-all` will NOT catch it, because the overflow isn't a single-frame local-array overrun past its own canary — it's a deep call chain whose cumulative frames exceed the (small) task stack, spilling into the adjacent task's saved registers.

**Why:** the hosted Linux/Android port uses far more stack per frame than bare-metal — glibc `sendto`/`recv`/`getaddrinfo`, HMAC-SHA256, and especially **fixed per-packet scratch buffers declared on the stack**. In `arch/linux/*/net_relay.c` there were four `unsigned char buf[MAX_PKT]` (MAX_PKT = 1416 B) locals (send_register, send_keepalive, net_relay_send, net_relay_recv). One 1.4 KB frame in a chain like `net_task → eth → ip → udp → swim_rx → swim_send → udp_send → net_relay_send` overflows a 2048/4096 T-Kernel stack. Bare-metal never hit it because there is no relay there.

**How to apply:**
- Make per-packet scratch buffers `static` in the cooperative kernel. Safe because no T-Kernel reschedule happens inside them (no `tk_*` call, syscalls don't trigger preemption — deferred to END_CRITICAL_SECTION). This matches the codebase's existing convention: `udp_send` uses `static UB udp_buf`, `pmesh_send` uses `static PMESH_DATA_PKT data_pkt`.
- Don't paper over it by bumping every task stack to 64 KB — wasteful, especially for the Android fleet. Find the oversized frame and move it off the stack.
- Fast diagnosis when no gdb: an async-signal-safe `SIGSEGV`/`SIGBUS` handler (SA_SIGINFO) that prints the faulting `pc` from the ucontext + `si_addr` raw-hex (NO `backtrace()` — it mallocs and re-faults on a corrupted heap). `pc==addr==garbage` ⇒ smashed return address ⇒ stack overflow. Then bisect: bump all task stacks huge; if the crash vanishes it's a stack-size problem, then hunt the big frame.

**RECURRENCE 2026-06-06 (wave 7 integration — same trap, NEW vector):** not a stack
*buffer* this time but **glibc `dprintf(2,...)` itself** — it funnels through `vfprintf`
which burns ~3960 B of stack. relay-HA added two failover-instant log lines
(`log_relay` "failover ->", "relay#N unresponsive") on the `net_task`/kdds-publisher
tasks (~4KB stacks). Overflowed only at the failover instant (steady-state send/recv
has no big frame). Deterministic garbage PC `0x2000000024` (two stack words misread as
a return addr — NOT in .text) on ALL nodes (identical binary→identical layout→identical
victim). Mis-blamed on guard/'dtr-worker' but the attribution was CORRECT: B-team's
guarded dtr-worker was just the deterministic victim of A-team's overflow — which is why
A's branch passed in isolation (no guard, no dtr-worker). Fix: hand-rolled static-buffer
`write(2)` logger (`rl_append`/`rl_append_dec`), byte-identical output. **Lesson upgraded:
on small T-Kernel task stacks, treat ANY glibc stdio (dprintf/printf/snprintf via
vfprintf) as a ~4KB stack hazard, not just `buf[MAX_PKT]` locals.** Boot-time prints on
the big main stack are fine; hot-path prints on worker tasks are not. Commit 4e53605.

Sibling traps in this port: [[feedback-lp64-typedef-trap]], [[feedback-lp64-allocator-trap]], [[feedback-x86_64-mcontext-fpregs]]. Surfaced by [[moment-2026-05-29-distributed-inference]].
