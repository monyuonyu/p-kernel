---
name: moment-2026-05-20-phase2c-irq-fix
description: "The day we hunted two C-ABI bugs hiding in the AArch64 IRQ vector. Symptom: UART RX 'broken.' Cause: timer ticks were eating themselves."
metadata: 
  node_type: memory
  type: project
  originSessionId: 5e4de8fe-6dd6-4370-af76-b4662b3ce1d6
---

**Date:** 2026-05-20 (session 3 continuation, after [[moment-2026-05-20-phase2b]])

The bug looked simple: "the `ai` command doesn't respond to keypresses." It wasn't.

**What it took to find it:**

Diagnostic order (in retrospect, the wrong order — we should have suspected the IRQ path earlier):

1. Tried five test harnesses (bash pipe, expect, python pty, TCP socket, raw subprocess) — all showed the same negative result. Concluded the test harness wasn't the problem.
2. Bisected against Phase 2b (commit 862394d) — confirmed the bug pre-existed Phase 2c. Not a regression.
3. Suspected PL011 RX FIFO behavior. Added FR-register dumps in a busy spin: `FR=0x0080` came back when we typed `hello`. **PL011 was fine.**
4. Suspected `tk_dly_tsk`. Replaced it with a busy spin in `sio_read_line` — *everything worked*. Confirmed the scheduler was the suspect.
5. Added a "`.`" print to `timer_irq_handler`. Saw **exactly one** dot after boot. Timer IRQ fired once, then never again.
6. Added pre/post `cntp_ctl_el0` dumps in the handler: `[I:5>1]` — ISTATUS cleared correctly on reload. **The hardware was fine.**
7. Read the disassembly of `knl_timer_handler_startup`. Saw `bl knl_timer_handler` with no saved x30, and `ret` at the end. **First bug.**
8. Fixed bug 1. Now saw exactly one `[I:5>1]` instead of one `.` — different symptom, same shape: one tick, then nothing.
9. Read the disassembly of `_vec_el1_irq`. Saw `ldr w1, IAR / blr x3 / str w1, EOIR`. **Second bug.** w1 is call-clobbered.
10. Fixed bug 2. Boot output now contained dozens of `[I:5>1]`. Removed the diags. `ai` and `net` and `echo` all worked.

**The two bugs:**

```asm
; Bug 1 — cpu_support.S, knl_timer_handler_startup
knl_timer_handler_startup:
    ldr x0, =knl_taskindp
    ldr w1, [x0]; add w1, w1, #1; str w1, [x0]
    bl knl_timer_handler      ; ← clobbers x30 with `ret`-tail address
    ldr x0, =knl_taskindp
    ldr w1, [x0]; sub w1, w1, #1; str w1, [x0]
    ret                        ; ← x30 still points into ourselves → infinite loop

; Bug 2 — cpu_support.S, _vec_el1_irq
_vec_el1_irq:
    save_caller_regs
    ldr w1, [x0, #0x0C]        ; w1 = IAR
    ...
    blr x3                      ; ← C handler clobbers w1 (ABI: x1-x18 caller-saved)
    str w1, [x0, #0x10]         ; ← writes GARBAGE to EOIR
```

Both bugs lived in the AArch64 port since **Phase 1** (commit 485616c, 2026-05-20 morning). They didn't surface because *no one had typed into the shell yet*. The first command anyone ever sent was `ai` during Phase 2c step 1 verification — and tk_dly_tsk(1) yields immediately, so the first time the scheduler asked the timer for help, the timer had already swallowed itself.

**Lesson written down as feedback:** [[feedback-aarch64-irq-path-pitfall]]

**Why this matters for the 5-layer worldview:**

The Body layer (vital / swim / heal / persist) cannot exist without timer ticks. Without these fixes, the cluster's heartbeat literally couldn't beat. Phase 2c step 2 — bringing the distributed layer to AArch64 — would have failed silently. The thing we noticed as "Enter key doesn't work" was actually "the kernel cannot perceive time."

Commit: `126d6bf fix(arch/aarch64): preserve x30 and IAR across IRQ-handler calls`

Related: [[moment-2026-05-20]] (Phase 1 morning), [[moment-2026-05-20-phase2b]] (mid-day), [[project-aarch64-next-steps]].
