---
name: feedback-aarch64-irq-path-pitfall
description: "When AArch64 input/timing \"doesn't work,\" check the IRQ vector and IRQ-time function calls before suspecting the device. Two C-ABI traps lurk."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 5e4de8fe-6dd6-4370-af76-b4662b3ce1d6
---

When AArch64 input or timing "doesn't work" — `tk_dly_tsk` hangs, shell input silently disappears, semaphores never wake — **look at the IRQ delivery path first**, not the device.

Two C-ABI traps to check whenever an IRQ handler in cpu_support.S calls into C:

1. **Save x30 before any `bl` / `blr`.** A function whose only "epilogue" is `ret` will silently loop into itself after the BL clobbers the link register. The first IRQ silently never completes; the kernel spins forever in the tail of the handler. Symptom from the outside: exactly one tick, then nothing.

2. **Don't keep the IAR value in a caller-saved register across a `blr` to a C handler.** w1-w18 are call-clobbered by the AArch64 ABI, so any C function will overwrite them. Writing a stale w1 to GICC_EOIR sends garbage to the GIC; the IRQ stays "active," and the GIC then filters out subsequent equal- or lower-priority interrupts. Symptom from the outside: the first tick works, every subsequent tick is dropped. Stash IAR on the stack across the call, or use a callee-saved register you've explicitly preserved.

**Why:** Both bugs hide in the AArch64 port and survived Phase 1 + Phase 2b because the only consumer of timer ticks at the time was `tk_dly_tsk(1)` in `sio_read_line`, and nobody had typed into the shell yet (the `net`/`ai` commands were never actually exercised interactively until Phase 2c). Each bug alone would look like "shell input doesn't work," masking the real failure mode.

**How to apply:** Anytime you see `tk_dly_tsk` / `tk_wai_sem` block longer than the timeout, before you debug the device driver, add a one-character print inside the timer IRQ handler (or a `static volatile unsigned irq_count` increment). If exactly one print appears, you have one of these two bugs.

Related: [[moment-2026-05-20-phase2c-irq-fix]] documents the diagnosis session.
