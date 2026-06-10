---
name: feedback-x86_64-mcontext-fpregs
description: "On x86_64-linux, mcontext_t.fpregs is a stale-after-sigreturn pointer; never memcpy a captured mcontext as-is. Patch only the named gregs."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: a9d29b54-c19f-4c9e-ae7c-cbd6237f6a81
---

**Rule:** When manipulating saved mcontexts inside a SIGALRM-as-IRQ
handler on x86_64-linux, do **not** wholesale-memcpy between
`uc->uc_mcontext` and a saved `mcontext_t`. Copy **only `gregs[]`**.
For "fresh task install" cases, patch only the specific gregs the
init function set (REG_R12/13/RSP/RIP for the trampoline contract);
leave segments, flags, fpregs, and __reserved1 alone.

**Why:** `mcontext_t.fpregs` on Linux/x86_64 is `struct _libc_fpstate *`
— a pointer into the live signal frame's kernel-allocated FP save
area. That buffer is freed when sigreturn unwinds the frame, so any
saved copy of the pointer dangles. Restoring it via memcpy later
either SIGSEGVs in sigreturn (kernel deref of stale FP state) or
silently uses garbage FP state.

On aarch64 the equivalent (FPSIMD context inside `__reserved`) is
**inline** — you can memcpy it freely between captured contexts —
but the bare zeroing-then-restore path still breaks (sigreturn
rejects sigframes without a valid FPSIMD section). Different
shape, same "don't fight what the kernel handed you" theme.

**How to apply:** the three primitives `save_into_ctx`,
`restore_populated`, `install_fresh` in
`arch/linux/x86_64/poc_preempt.c` (and eventually `preempt.c`) all
operate solely on gregs[]. Any future addition that needs to save FP
state must do so via its **own** buffer (e.g. read fpregs's contents
through the live pointer at save time, store in arch_full_ctx_t,
write back through a *new* uc's fpregs at restore time) — not via
the kernel-supplied pointer.

This applies to any future hosted port that uses `siginfo_t` + mcontext
rewriting on x86_64. Likely siblings: macOS x86_64 (Mach surface
differs but has the same stale-pointer concern), FreeBSD x86_64.

Cross-link: [[moment-2026-05-21-x86_64-sessions12]] captures the
session this was learned in. The aarch64 sibling rule lives in the
session-2 part of [[project-linux-userspace-port]].
