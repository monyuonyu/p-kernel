---
name: moment-2026-05-21-first-pkernel-on-x86_64
description: "The day p-kernel ran end-to-end as a Linux x86_64 process — single push, no debug session needed. aarch64 lessons paid off."
metadata: 
  node_type: memory
  type: project
  originSessionId: a9d29b54-c19f-4c9e-ae7c-cbd6237f6a81
---

**Date:** 2026-05-21 (very late, session 6)

**What happened.** A few hours after [[moment-2026-05-21-first-pkernel-on-linux]] (the aarch64 Linux port booting for the first time), the x86_64 sibling lit up on the first complete build attempt. No SIGILL, no SIGSEGV, no allocator hang — straight to the shell prompt.

```
$ qemu-x86_64 ./p-kernel
=== p-kernel linux boot ===
[INIT] termios stdin/stdout
[BOOT] Starting T-Kernel...
[T-Kernel] Initial task started
 p-kernel  [linux / x86_64 userspace]
[ai]   Tensor pool   : 16 slots × 16 KB
[kdds] K-DDS ready  port=7376
[dtr] Transformer initialized   params: 568 floats
  T-Kernel is alive inside a Linux process.
p-kernel>
```

**Why this is a moment.** The aarch64 port (same day, earlier) took 3 sessions and required hunting two real bugs ([[moment-2026-05-21-first-pkernel-on-linux]]: DI/EI macro shadow + SP 16-alignment). The x86_64 port took 3 sessions of equivalent code volume but **needed zero post-build debugging** — the LP64 typedef fix (`UW long → int`), the LP64 allocator fix (PTR_UINT), the SP alignment guarantee in knl_setup_context, the DI/EI flag-based shadow — all were already in place from the earlier work. The shared T-Kernel surface is genuinely portable across LP64 sibling ABIs.

**The road, 3 commits in one session:**
- `9c6d588` Session 3a — dispatcher port (cpu_support.S + poc_dispatch.c verification)
- `572c443` Session 3b/c/d combined — kernel integration + arch/common linked + boot/linux_x86_64/

**Headline files added:**
- `arch/linux/x86_64/cpu_support.S` (sibling of aarch64's)
- `arch/linux/x86_64/include/{cpu_status,cpu_insn,cpu_task,syslib_depend,tkdev_timer,offset}.h`
- `arch/linux/x86_64/{cpu_init,tkdev_init,preempt,time,sio,net_unix,rtl8139,pci,arch_reboot,vfs_stub,inittask_def,inittask_main,usermain}.c`
- `boot/linux_x86_64/{Makefile,main.c}`
- 6 T-Kernel common headers patched to recognise `_APP_X86_64_`

**Lessons that ported clean across ABIs:**
- LP64 typedef trap (`UW=long` → 8 bytes, must be `int`)
- LP64 allocator trap (`(UW)ptr` truncates; use `(PTR_UINT)ptr`)
- 16-byte SP alignment in knl_setup_context (round-down isstack)
- DI/EI macro shadow (overrides privileged-instruction inline)
- mcontext fpregs pointer stale-after-sigreturn ([[feedback-x86_64-mcontext-fpregs]])

**The frame size difference (worth knowing):**
- AArch64 dormant frame: 112 bytes (12 callee-saved x19-x30 + taskmode + padding)
- x86_64 dormant frame: 64 bytes (6 callee-saved + rip slot + taskmode)
- TCB offsets identical (TCB_SSP=192) because the *surrounding* TCB struct uses LP64-uniform alignment.

**What's not done (deliberately, for now):**
- Native x86_64 host build (only verified cross-compiled + qemu-user); should "just work" on a real x86_64 box but unverified.
- Two-node mesh on x86_64 (UDP loopback infrastructure is in place, never exercised on this arch).
- Cleaner include path — currently `arch/linux/x86_64/include` plus `arch/aarch64/include` is in scope to share LP64-uniform headers (sysdef_depend, sysinfo_depend, etc.). It works but the "borrow from aarch64" feels off; a cleaner refactor would move those into `arch/common/include/lp64/` or similar.

**Why the speed?** Every previous bug found on aarch64 became a static-assert or design rule that the x86_64 port inherited automatically. The shared `kernel/common/` and `arch/common/` code paths really are portable. The "AI lives where the cluster lives" thesis becomes one step more believable: x86_64 servers, aarch64 phones / Raspberry Pis, all running the same kernel.

Cross-link to [[project-linux-userspace-port]] — the parent project memory; should be updated to note both arches are now in.
