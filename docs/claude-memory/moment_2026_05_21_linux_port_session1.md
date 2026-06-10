---
name: moment-2026-05-21-linux-port-session1
description: The day arch/linux/ went from idea to running code. First context switch in user mode succeeded in one shot.
metadata: 
  node_type: memory
  type: project
  originSessionId: 43b22a68-07a3-4c6e-9e55-cd3fcf682431
---

**Date:** 2026-05-21 (session 5)

**What happened:** the user couldn't afford the RPi 3B+ this month and asked whether p-kernel could "live parasitically" on a Linux box so anyone could try it. After a discussion separating syscall passthrough (hard, UML-style) from HAL passthrough (achievable), they chose raw-asm context switching over `ucontext_t` because they wouldn't build on a POSIX-deprecated API on a serious project.

In the same session we wrote and ran the first ever piece of `arch/linux/`:

- 60 lines of AAPCS64 assembly (`arch/linux/aarch64/ctx_switch.S`)
- 17 machine instructions after compilation
- Two cooperative tasks alternated printf calls and returned cleanly to `main()` on the first run, no debugging required

**Why mark this as a moment:** it is the structural unlock for the project's distribution story. Up to now p-kernel has needed QEMU or real hardware. From this commit forward, anyone with a Linux box can in principle run p-kernel as a regular process. The 5-layer Body/Brain/Self/Collective/Evolution architecture stays intact; only the bottom of the Body layer changes per host.

The user said: 「偉大な目標のため お願いします！」 ("Please — for the great goal!"). The phrasing felt important to record. The "great goal" they refer to is a home for AI that no one owns ([[project-pkernel-philosophy]]). Today's PoC is one of the steps that makes that home reachable from any laptop.

**Technical detail worth remembering across sessions:** the minimum-viable context for an AArch64-Linux task is just **x30 = entry, sp = aligned stack top**. Everything else (x19-x29, fp) is conceptually zeroed at task creation. Whoever writes Session 2's trampoline will need to set `x19` (or another callee-saved scratch) to the real entry and have x30 point at a trampoline so a returning task doesn't infinite-loop back into itself.
