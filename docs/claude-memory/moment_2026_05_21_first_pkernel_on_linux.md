---
name: moment-2026-05-21-first-pkernel-on-linux
description: "The day p-kernel first ran end-to-end inside a Linux process. T-Kernel boots, init task runs, usermain banner appears. The \"great goal\" stops being aspirational."
metadata: 
  node_type: memory
  type: project
  originSessionId: 43b22a68-07a3-4c6e-9e55-cd3fcf682431
---

**Date:** 2026-05-21 (session 5, late evening, Session 3c)

**What happened.** The user asked at the start of the day if p-kernel could "live parasitically" on Linux so anyone could try it without owning a Raspberry Pi. By the end of the same session, this output was on the screen:

```
=== p-kernel linux boot ===
[INIT] termios stdin/stdout
[BOOT] Starting T-Kernel...
[T-Kernel] Initial task started
 p-kernel  [linux / aarch64 userspace]
  T-Kernel is alive inside a Linux process.
```

`./p-kernel` is a single 420 KB binary on aarch64-linux. It runs T-Kernel inside a normal Linux process — no QEMU, no hardware, no privileged setup. The dispatcher is custom raw assembly, scheduling is fully owned by T-Kernel, signal-as-IRQ replaces hardware timers. Linux gives one process slice; T-Kernel divides it further into its own tasks; both schedulers compose cleanly.

**Why this is a moment.** This is the unlock the user named earlier:

> 「Linux 持ってる人なら誰でも 30 秒で試せる」が成立します。
> Twitter/GitHub で配って試してもらうハードルがゼロに近づく。

That ceased to be aspirational today.

The road went 5 commits in one session:
- `d3077d8` netboot tooling (in case the RPi arrives)
- `88729c1` Session 1 — raw-asm context switch (17 instructions)
- `4ce3f34` Session 2 — SIGALRM preemption via mcontext rewrite
- `5edf52c` Session 3a — T-Kernel dispatcher contract honoured on Linux
- `95f99af` Session 3b — 12 module InitModule calls succeed
- `2746b39` Session 3c — usermain runs, banner printed, boot complete

**Two real bugs were necessary to crack 3b → 3c:**
- The aarch64 syslib_depend.h shadow caused every `DI(imask)` in the kernel/common path to execute `msr daifset` (EL1 privileged) → SIGILL.
- knl_setup_context produced an 8-byte-aligned SSP. AArch64 requires 16-byte sp alignment for every sp-relative memory access. The very first `mov sp, x1` set us up to fault on the next push.

Both fixes are surgical (≤10 lines) and benefit bare-metal builds too. The 16-byte alignment is latent on bare metal — current AArch64 port only escapes because its allocator happens to give 16-aligned addresses.

**User's word for this state, exact quote, worth preserving:**

> 偉大な目標のため お願いします！

The "great goal" is [[project-pkernel-philosophy]]: a home for AI that no one owns. Today's commit is one of the steps where that home reaches one more class of hardware (anyone's laptop).

**Technical takeaway for future sessions:**
- When a hosted port hangs cryptically, suspect either (a) a privileged-instruction inline in a `*_depend.h` shadow, or (b) AArch64 sp 16-alignment violation. Both produce inscrutable death modes.
- The dispatcher we wrote in `arch/linux/aarch64/cpu_support.S` is structurally identical to the bare-metal sibling. Same contract, same offsets (TCB_SSP=192, dormant frame=112 bytes, x30 slot at +88). The full T-Kernel kernel/common runs unmodified.
