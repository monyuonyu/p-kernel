---
name: moment-2026-05-21-x86_64-sessions12
description: x86_64-linux sibling port — Sessions 1 (cooperative ctx_switch) and 2 (SIGALRM preemption + trampoline) done in one go. Verified on qemu-user from an aarch64 host.
metadata: 
  node_type: memory
  type: project
  originSessionId: a9d29b54-c19f-4c9e-ae7c-cbd6237f6a81
---

**Date:** 2026-05-21 (late evening, session 6)

**What happened.** After landing time-domain fix, LP64 allocator fix
(via worktree agent), and SWIM 2-node segfault triage, the day kept
going and built the x86_64-linux sibling foundation. By the end of
the session, two qemu-x86_64 runs were producing this:

```
$ make run-ctx
qemu-x86_64 ./poc_ctx_switch
[main] host=x86_64-linux, stack_a top=0x..., stack_b top=0x...
[main] entering task A
[A] counter=0 / [B] counter=1 / ... / [B] limit reached
[main] back in main — PoC complete

$ make run-preempt
qemu-x86_64 ./poc_preempt
[main] arming 10 ms timer, tick_limit=30
[main] ticks=30, count_a=28497727, count_b=27788843
[main] both tasks made progress — preemption OK
```

These are the structural twins of the aarch64 PoCs from earlier the
same day. Sessions 1+2 done; Session 3 (T-Kernel dispatcher port +
full kernel integration) is the next big lift.

**Files (all under `arch/linux/x86_64/`):**
- `ctx_switch.S` — 19 instructions / 0x40 bytes. System V AMD64 callee-
  saved set (rbx, rbp, r12-r15, rsp) save+load + ret.
- `trampoline.S` — `task_trampoline`: `pushq $0` for ABI alignment,
  `movq %r13, %rdi` (arg), `call *%r12` (entry), fall-through to
  `task_exit`.
- `poc_ctx_switch.c` — cooperative 2-task driver; init_ctx writes
  entry to the 16-aligned slot at sp_top-16.
- `poc_preempt.c` — SIGALRM-driven preemption via mcontext.gregs[]
  rewrite; siglongjmp back to main after tick_limit ticks.
- `Makefile` — auto-detects host arch; cross-compiles via
  `x86_64-linux-gnu-gcc` and runs via `qemu-x86_64` when not native.

**Cross-build tooling discovered:**
- `apt install gcc-x86-64-linux-gnu libc6-dev-amd64-cross` gives the
  toolchain. The driver is named `x86_64-linux-gnu-gcc` (not `cc` —
  the Makefile initially assumed `cc` and broke).
- `qemu-x86_64` (already installed for other reasons) runs static
  x86_64 ELFs directly: `qemu-x86_64 ./poc_preempt`.

**Critical x86_64-specific lesson:**

`mcontext_t.fpregs` is a *pointer*, not inline data. On aarch64 the
equivalent (FPSIMD section inside `__reserved`) is inline; you can
memcpy it freely between captured mcontexts. On x86_64 the pointer
identifies a kernel-allocated buffer for the live signal frame, and
it goes stale the moment sigreturn fires. So:

- `save_into_ctx` copies **only gregs[]**. Not fpregs, not __reserved1.
- `restore_populated` copies **only gregs[]**. Same reason.
- `install_fresh` patches **only the named gregs** (REG_R12, R13, RSP,
  RIP) and leaves uc's live tail (segments, flags, FP pointer)
  untouched.

The general rule that survives across hosted ports: **"touch the
named regs; touch nothing the kernel handed you."** This is the
analogue of aarch64's "round-trip captured mcontexts; install fresh
ones via partial patch."

**No bug surprises this round.** Sessions 1+2 on x86_64 went straight
through with no false starts — the aarch64 experience (16-byte sp
alignment, fresh mcontext install rules, IRQ-disabled flag drain
strategy) was directly applicable, and the lessons captured in
[[moment-2026-05-21-first-pkernel-on-linux]] + the linux port project
notes paid off immediately. Total code: 232 + 350 = 582 lines.

**Session 3 sketch for future work** (see [[project-linux-userspace-port]]
for the parallel aarch64 history):
- `arch/linux/x86_64/include/cpu_status.h` / `cpu_insn.h` shadows
  (override the privileged-instruction macros that bare-metal x86
  uses).
- `arch/linux/x86_64/cpu_support.S` — T-Kernel dispatcher port. The
  bare-metal sibling lives in `arch/x86/`, but it's 32-bit only;
  the 64-bit shape will need designing from scratch (System V
  callee-saved set, different TCB offsets potentially — though if
  the TCB ABI is host-arch-conditional already, those carry over).
- `arch/linux/x86_64/{cpu_init,tkdev_init,preempt,time,sio,net_unix,
  rtl8139,pci,arch_reboot,vfs_stub,inittask_*,usermain}.c` —
  largely mirror the aarch64/ TUs with x86_64-specific bits.
- `boot/linux/Makefile` gains an `ARCH=x86_64` switch, or split into
  `boot/linux_x86_64/`.
