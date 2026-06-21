# ②.2b — true async register-context preemption + secondary timer/WAIT wiring: implementation plan (cert-first)

**Status: DESIGN PLAN on trunk `ed63e951`** (②.0 + ②.1a + ②.1b + ②.2a all merged; `git log -1`: `ed63e951 backlog: ②.2a shipped — production scheduler is SMP (the heart of the final goal); ②.2b/②.2c next`). Read-only on code; no implementation in this wave. This plan makes ②.2b **READY and de-risked** on top of the shipped ②.2a production-scheduler-SMP slice — it does **not** start it. ②.2b is the **single most C-ABI-fault-prone wave in the repo** (an asynchronous register-context switch FROM interrupt context, threaded through the aarch64 IRQ vector — the repo's recurring failure mode), and is **awaiting mk_pino's go-ahead**, via a **separate impl→audit cycle** (implementer ≠ auditor ≠ commander; the development METHOD is the project's immune system). All file:line grounding below is against the shared checkout at `ed63e951`.

> Base verified: `git rev-parse --short HEAD` == `ed63e951`; `arch/aarch64/smp_prod.c` + `arch/aarch64/include/smp_percpu.h` both exist (the ②.2a artifacts). The two earlier design docs (`smp-2-production-scheduler-plan.md`, `smp-1-ipi-preempt-plan.md`) were written at *earlier* bases (`fd4900a9` / `9db8a998`); where their line numbers disagree with the shipped code, **this plan grounds against `ed63e951`** (e.g. `_vec_el1_irq` is now `cpu_support.S:301-351`, `knl_dispatch` is `cpu_support.S:101-186`, `TCB_SSP == 192` per `arch/common/include/lp64/offset.h:88`).

---

## 0. LEAD WITH THE CERT + THE aarch64 IRQ-PATH C-ABI TRAP (the repo's recurring failure mode)

### 0.1 The cert, stated first (cert-first)

**`[smp-async-preempt]`** — a secondary CPU's task is preempted **ASYNCHRONOUSLY mid-computation** — it spins in a tight loop that **NEVER checks any flag** (the load-bearing difference from ②.1a's cooperative-at-a-checkpoint preempt, `smp.c:717-767`) — by a higher-priority task readied on CPU 0 and an SGI sent to the secondary; and the low-prio task is **proven suspended mid-loop and later RESUMED correctly** (its loop counter continues from where it was interrupted; full register state intact). Print **`SMP-ASYNC-PREEMPT: PASS`**.

> **FALSIFIER (MUST go RED): `-DSMP_NO_ASYNC`** — revert the IRQ-return path to the ②.1a behavior (the SGI handler only sets `g_resched_pending[me]`, NO context switch on the IRQ-return path). The tight loop has no flag-check, so it is **NEVER preempted** → the high-prio task never runs on the secondary → the driver watchdog elapses → **`SMP-ASYNC-PREEMPT: FAIL`**. This proves the cert is non-vacuous: the mid-loop preempt happens **only because** the IRQ-return path performs a real register-context switch.

**`[smp-secondary-sleep]`** (②.2b-ii) — a real T-Kernel task running on the **secondary** calls `tk_dly_tsk(n)` (the path that FAULTS today, `smp_prod.c:88-99`), BLOCKS, and **WAKES correctly** after the delay; print **`SMP-SECONDARY-SLEEP: PASS`**. FALSIFIER: `-DSMP_NO_SEC_TIMER` (don't enable the secondary's own CNTP PPI / don't drive its tick) → the secondary's delay never expires → the task never wakes → watchdog **FAIL**.

Both are QEMU `-smp 4`-testable; the **barrier/IRQ-timing teeth are `[live]`-only on RPi3** (§3.6, §6).

### 0.2 The aarch64 IRQ-path C-ABI trap (lead with the dominant risk)

The repo's standing rule (MEMORY.md, `feedback_aarch64_irq_path_pitfall`): *"When input/timing doesn't work on aarch64, suspect the IRQ vector before the device. Two C-ABI traps recur."* ②.2b lives **entirely inside that trap**, because it makes `_vec_el1_irq` (`cpu_support.S:301-351`) — which today only `save_caller_regs` → dispatch → EOIR → `restore_caller_regs` → `eret` — perform a **register-context switch to a different task's stack** before returning. The specific traps ②.2b must not trip, each grounded:

1. **The reserved 16-byte IAR slot must survive the switch.** `_vec_el1_irq` does `sub sp, sp, #16; str w1, [sp]` (`cpu_support.S:307,324`) to stash the GIC IAR across the handler `blr`, and reloads it at `.Lirq_eoi` (`ldr w1, [sp]; add sp, sp, #16`, `cpu_support.S:340-341`) to write `GICC_EOIR` (`:347`). If the context switch happens **before** EOIR, the switch lands us on a *different* stack where that slot does not exist → the EOIR reads garbage → the GIC never deactivates the SGI → no further IRQ is ever delivered to that CPU. **Rule: EOIR the SGI on the OLD (interrupted) context's behalf, BEFORE the context switch** (§3.3).
2. **The 160-byte `save_caller_regs` frame vs the 112-byte `knl_dispatch` frame must nest, not collide.** `save_caller_regs` (`cpu_support.S:244-256`) pushes a 160-byte caller-saved frame (x0..x18,x30) onto the **interrupted task's** stack. `knl_dispatch` (`cpu_support.S:106-115`) pushes a **separate** 112-byte callee-saved frame (x19..x30 + taskmode) and stores SP into `CUR_CTXTSK->ssp`. The interrupted task's resume state therefore lives in **two nested frames** on its own stack: the 112-byte dispatcher frame on top of the 160-byte IRQ frame. When that task is later re-dispatched, `.Ldispatch_loop` restores the 112-byte frame and `ret`s — but the `ret` must return into the **IRQ-return tail** so `restore_caller_regs` + `eret` then unwind the 160-byte frame and `eret` back to the exact interrupted PC. Getting this nesting wrong is the classic "garbage-PC crash (pc==addr==random)" the repo has hit before (MEMORY.md `feedback_hosted_relay_stack_overflow` names the symptom). §3.2 pins the exact hook point that makes the two frames nest correctly.
3. **`daif` state at the hook.** `knl_dispatch` masks IRQ+FIQ on entry (`msr daifset, #0x3`, `cpu_support.S:103`) and `.Ldispatch_loop` clears them before `ret` (`msr daifclr, #0x3`, `:179`). The IRQ vector is already running with IRQs masked (hardware masks on exception entry). The hook must not double-unmask or leave the new task running with IRQs wrongly masked.
4. **The handler must stay ABI-clean.** `smp_resched_sgi_handler` (`smp.c:258-266`) is a leaf `void(void)` invoked by `blr x3` (`cpu_support.S:336`). ②.2b does **not** make the handler itself switch context (that is Option B, deferred — §3.5). The switch is done by the vector tail in asm, after the handler returns. Keep the handler a flag-set leaf.

**This §0.2 trap analysis is what the auditor must independently re-derive against the running QEMU** — the failure, if any, will be here (a clobbered IAR slot, a mis-nested frame, or a daif mismatch), not in the "policy" C code.

---

## 1. Where ②.2a left us, and the two things ②.2b adds (the boundary)

**②.2a shipped** (`ed63e951`): the **production** dispatcher is SMP. `cpu_support.S:101-186` (`knl_dispatch` / `.Ldispatch_loop`) per-CPU-izes `knl_ctxtsk`/`knl_schedtsk` via `LD_PERCPU_BASE` (`cpu_support.S:67-75`) under `SMP_SELFTEST`; `CUR_CTXTSK`/`CUR_SCHEDTSK`/`CUR_DISPATCH_DISABLED` (`arch/aarch64/include/cpu_status.h:24-26`) resolve to `g_smpcpu[smp_this_cpu()].{...}` under SMP and to the plain globals otherwise; `END_CRITICAL_SECTION` routes through the BKL and tests `CUR_CTXTSK != CUR_SCHEDTSK` (`cpu_status.h:54-62`); `knl_reschedule` is CUR_*-based (`task.h:268-277`). `smp_prod.c` proves `[smp-2tasks-prod]`: two REAL TCBs run on two CPUs under the BKL (`smp_prod_test_run`, `smp_prod.c:115-185`), the secondary entering the production dispatcher via `smp_prod_enter_dispatch` (`cpu_support.S:500-502` → `b .Ldispatch_loop`).

**The two honest gaps ②.2a left, stated verbatim in the code:**

- **No async preempt.** The ②.1a SGI handler (`smp_resched_sgi_handler`, `smp.c:258-266`) **only sets `g_resched_pending[me]`** and returns; the actual reschedule is cooperative-at-a-checkpoint (the low-prio task polls the flag at loop boundaries, `smp.c:735-767`). The ②.1a doc states the deferral: *"A TRUE asynchronous register-context preempt inside the SGI handler is the production context-switch work, DEFERRED to ②.2"* (`smp.c:689-691`).
- **No secondary timer/WAIT.** ②.2a's task B parks on a bare `wfe`, **never a blocking syscall**, with the gap documented in `smp_prod.c:86-98`: *"the secondary's TIMER/WAIT path (its own EL1 timer PPI + tick-driven reschedule) is NOT wired in ②.2a — that is the ②.2b work … B must NOT call a blocking/timer syscall here (tk_dly_tsk/tk_slp_tsk would enter the unwired secondary wait path and fault)."*

**②.2b adds exactly these two, and ONLY these:**
1. **§2 / §3 — true async register-context preempt:** the IRQ-return path performs a real register-context switch (`knl_dispatch`-equivalent save/restore) when `CUR_CTXTSK != CUR_SCHEDTSK` on the interrupted CPU — a preemptive switch *from interrupt context*, no cooperation, no polling.
2. **§4 — the secondary's timer + WAIT path:** a secondary task can `tk_dly_tsk`/`tk_slp_tsk` (block) and wake — via its own per-CPU CNTP PPI tick (or boot-CPU-tick-drives-all, §4.2) and a cross-CPU wakeup that re-dispatches the now-ready task on its CPU.

**The boundary ②.2b must hold (unchanged from ②.2a, restated as a hard contract):** §5 — N=1/default build BYTE-IDENTICAL (everything `SMP_SELFTEST`-gated); the production tick/WAIT on the boot CPU at N=1 unchanged; the mind untouched; the BKL still serialises kernel state.

---

## 2. The determinism + the existing save/restore ②.2b reuses (ground the mechanism)

### 2.1 The production save/restore is the SAME switch the IRQ-return path will reuse

The cleanest, lowest-risk ②.2b **reuses the already-proven `knl_dispatch` save/restore** rather than writing a second context switch (the §0.2 trap counsels minimizing new asm). The two halves already in the tree:

- **SAVE** (`knl_dispatch`, `cpu_support.S:101-135`): masks IRQ/FIQ (`:103`), pushes the 112-byte callee-saved frame (`stp x19..x30`, taskmode, `:106-115`), then under SMP loads `&g_smpcpu[me]` (`LD_PERCPU_BASE x2,x3`, `:119`), reads `CUR_CTXTSK` (`:120`), and stores the current SP into `CUR_CTXTSK->ssp` (`str x1,[x0,#TCB_SSP]`, `:123`; `TCB_SSP==192`, `offset.h:88`), then NULLs `CUR_CTXTSK` (`:125`). **This is the exact "suspend the interrupted task" operation ②.2b needs** — it captures everything `restore` will need.
- **RESTORE** (`.Ldispatch_loop`, `cpu_support.S:140-180`): under SMP loads `CUR_SCHEDTSK` (`:143`), stores it into `CUR_CTXTSK` (`:145`), loads its saved SP (`ldr x1,[x0,#TCB_SSP]; mov sp,x1`, `:147-148`), clears per-CPU `dispatch_disabled` (`:150`), restores the 112-byte frame + taskmode (`:167-177`), unmasks IRQ/FIQ (`:179`), and `ret`s into x30 (the task's resume PC or the trampoline, `:180`).

**The key reconciliation ②.2b must get right (the §0.2 trap #2):** `knl_dispatch` was written to be called from *task context* via `END_CRITICAL_SECTION` (`cpu_status.h:54-61`), where x30 already holds the caller's return address. From the **IRQ-return path**, the interrupted task's *true* PC is in `ELR_EL1`, and its caller-saved regs (x0..x18,x30) are in the 160-byte `save_caller_regs` frame, NOT yet committed to `knl_dispatch`'s 112-byte frame. So the switch cannot be a naive "call `knl_dispatch`" — §3.2 specifies exactly where it hooks so the 160-byte IRQ frame **stays on the interrupted task's stack** and is unwound by `restore_caller_regs`+`eret` when that task is re-dispatched.

### 2.2 The mind is untouched; the BKL still blankets kernel state

②.2b adds no mind math and reads/writes no mind weights, engram ring, or `gl_merge`. The async switch and the secondary tick both run with the BKL serializing kernel-state mutation (§5.4). The `[smp-one-mind]` crown is **②.2c** (deferred, §7) — it needs the byte-identity proof of `r_forward` under SMP, which ②.2b does not deliver. ②.2b's only crown obligation is the negative one: don't regress default byte-identity, don't corrupt kernel state under a real async preempt.

---

## 3. True async register-context preempt (Option A — the core, §3.2 pins the hook)

### 3.1 Option A vs Option B (Option A is PREFERRED — restate why)

The ②.2 design doc (`smp-2-production-scheduler-plan.md §3.2`) names two shapes:

- **Option A — "dispatch on IRQ return", reusing `knl_dispatch`, mirroring `END_CRITICAL_SECTION`'s condition.** The SGI handler keeps setting `g_resched_pending[me]` (as today). The **IRQ-return tail**, *after EOIR* and *before* `restore_caller_regs`/`eret`, checks the SAME 4-clause condition `END_CRITICAL_SECTION` uses (`cpu_status.h:54-61`): `g_resched_pending[me]` AND `CUR_CTXTSK != CUR_SCHEDTSK` AND `!CUR_DISPATCH_DISABLED` AND `!knl_isTaskIndependent()`; if all hold, switch context via the `knl_dispatch` save/restore instead of returning to the interrupted task. **PREFERRED** — it reuses the proven save/restore and is the standard T-Kernel "dispatch-on-interrupt-return" model generalized per-CPU; it is the lowest-risk path past the §0.2 trap.
- **Option B — full async switch inside the SGI handler.** The handler itself reconciles the 160-byte `save_caller_regs` frame with `knl_dispatch`'s 112-byte frame. Higher risk (exactly the C-ABI/stack-layout reconciliation §0.2 warns about). **DEFERRED unless A proves insufficient** (§3.5).

**②.2b implements Option A.**

### 3.2 WHERE in `_vec_el1_irq` the switch hooks (the precise, load-bearing answer)

Today `_vec_el1_irq` (`cpu_support.S:301-351`) is:

```
_vec_el1_irq:
    save_caller_regs            ; :302  push 160-byte caller frame on INTERRUPTED task's stack
    sub sp, sp, #16             ; :307  reserve IAR slot
    ... read GICC_IAR -> w1, str w1,[sp] ...   ; :321-324
    ... dispatch knl_intvec[INTID] via blr x3 ...  ; :331-336  (the SGI handler runs here)
.Lirq_no_dispatch:
.Lirq_eoi:
    ldr w1, [sp]                ; :340  reload IAR
    add sp, sp, #16             ; :341  pop IAR slot
    str w1, [gicc_base_ptr+0x10]; :345-347  GICC_EOIR  (deactivate the SGI)
    restore_caller_regs         ; :350  pop 160-byte caller frame
    eret                        ; :351  return to interrupted PC
```

**The hook goes BETWEEN the EOIR (`:347`) and `restore_caller_regs` (`:350`)** — i.e. immediately after the SGI is deactivated at the GIC, while we are still on the **interrupted task's stack** with the 160-byte caller frame still present and `sp` already past the popped IAR slot. The new tail (gated `SMP_SELFTEST`):

```
.Lirq_eoi:
    ldr w1, [sp]
    add sp, sp, #16
    str w1, [x0, #0x10]          ; GICC_EOIR — EOIR on the OLD context's behalf (trap #1)

#ifdef SMP_SELFTEST                ; ②.2b async-preempt hook (Option A)
    bl   smp_irq_need_resched      ; C: returns 1 iff g_resched_pending[me] && CUR_CTXTSK!=CUR_SCHEDTSK
                                   ;    && !CUR_DISPATCH_DISABLED && !knl_isTaskIndependent()
                                   ;    (mirrors END_CRITICAL_SECTION; consumes the pending flag)
    cbz  x0, .Lirq_return          ; no switch needed → normal return
    restore_caller_regs            ; UNWIND the 160-byte caller frame back into the interrupted
                                   ; task's live regs (x0..x18,x30) — so knl_dispatch's 112-byte
                                   ; frame is taken over a CLEAN register state, exactly as if the
                                   ; task had reached END_CRITICAL_SECTION. The interrupted PC is
                                   ; ELR_EL1; we capture it as the resume point below.
    ... (see §3.3 for the ELR handling) ...
    b    knl_dispatch              ; SAVE interrupted ctx -> CUR_CTXTSK->ssp; RESTORE CUR_SCHEDTSK
.Lirq_return:
#endif
    restore_caller_regs
    eret
```

**Why the hook is here and not earlier:** EOIR must run first (trap #1 — the IAR slot still exists at `:340-347`, and the SGI must be deactivated before we abandon this stack frame). The switch must run **after** the caller frame is back in registers (`restore_caller_regs`) so that `knl_dispatch`'s 112-byte save captures the task's true live x19..x30 — not the vector's scratch. The two frames then nest as: when the interrupted task is later re-dispatched, `.Ldispatch_loop` restores its 112-byte frame and `ret`s to the **resume PC** we recorded (§3.3), which re-enters the interrupted instruction stream.

### 3.3 How the IRQ frame and the TCB context frame interact (the ELR reconciliation)

This is the single subtlest point. `knl_dispatch`'s frame stores x19..x30 and resumes via `ret` to x30 (`.Ldispatch_loop:180`). But a task preempted **mid-computation** was NOT at a function-call boundary — its true resume point is `ELR_EL1` (the PC the IRQ interrupted), and its full caller-saved register set (x0..x18) must also be restored. Two correct sub-designs (impl picks one; the auditor re-derives):

**(A-i) Trampoline-resume (the clean one).** Before `b knl_dispatch`, synthesize x30 = address of a tiny **`.Lirq_resume` trampoline** and let `knl_dispatch` save it as the task's resume PC. When the task is re-dispatched, `.Ldispatch_loop` `ret`s into `.Lirq_resume`, which re-establishes the IRQ-return epilogue for *that* task: it re-pushes / re-reads the saved ELR_EL1 + SPSR_EL1 (captured into the 112-byte frame's spare slot — note `cpu_support.S:18` shows `[sp+104]` is padding today, available for ELR) and the caller-saved x0..x18, then `eret`s to the interrupted PC. **This requires the 112-byte frame to also carry ELR_EL1/SPSR_EL1** (extend the saved frame for the IRQ-preempt path, or save them into the TCB) — the auditor must confirm the frame layout is consistent for BOTH a cooperatively-dispatched task and an async-preempted one, or they diverge on resume.

**(A-ii) Two-frame nesting (reuse, no new trampoline).** Do NOT `restore_caller_regs` before the switch; instead make `knl_dispatch` save the SP **as it stands with the 160-byte IRQ frame + the saved ELR/SPSR still on the stack**, and set the resume x30 to a label that does `restore_caller_regs; eret`. On re-dispatch, `.Ldispatch_loop` restores the 112-byte frame, `ret`s to that label, which unwinds the 160-byte caller frame and `eret`s to ELR_EL1. This keeps the two frames literally nested (112 over 160) and never reconstructs registers — but it requires saving ELR_EL1/SPSR_EL1 into the IRQ frame at vector entry (a small `save_caller_regs` extension) so the eret target survives the round-trip.

**Either way, the load-bearing requirement (and the auditor's falsification target):** ELR_EL1 + SPSR_EL1 (the interrupted PC + processor state) MUST be captured at IRQ entry and restored on resume, because a mid-loop preempt has no call-boundary x30 to return to. The ②.1a cooperative path never needed this (it preempted at a checkpoint where x30 was a valid return). **This is the new state ②.2b must thread, and it is exactly the C-ABI reconciliation §0.2 trap #2 names.** The `[smp-async-preempt]` cert (§3.6) is the proof it is correct: the loop counter resuming mid-value is impossible unless ELR/SPSR/x0..x18 all round-tripped.

### 3.4 `knl_taskindp` / `knl_dispatch_disabled` per-CPU caveat (a real ②.2a residual)

The Option-A condition includes `!knl_isTaskIndependent()` (`cpu_insn.h:66-69`, reading `knl_taskindp`, `cpu_insn.h:64`). **`knl_taskindp` is still a single GLOBAL `W` at `ed63e951`** (`cpu_init.c:17` — `W knl_taskindp = 0;`), NOT per-CPU, even though the ②.2 design doc §2.2 listed it as a per-CPU candidate. For ②.2b's narrow cert this is benign (the secondary's preempt loop is plain task context, `knl_taskindp==0`), but **the plan flags it explicitly**: the IRQ-return condition reads a global that, under genuine concurrent IRQ handling on two CPUs, is shared. `knl_dispatch_disabled` WAS made per-CPU (`CUR_DISPATCH_DISABLED` → `g_smpcpu[me].dispatch_disabled`, `smp_percpu.h:67`, `cpu_status.h:26`). **②.2b must either (a) make `knl_taskindp` per-CPU too (small, mirrors the dispatch_disabled work), or (b) prove the cert never enters task-independent context on the secondary and ledger the global as a `[live]`-deferred sharpening.** The auditor must not let this slide silently — it is precisely the "cert must cover ALL paths" lesson (MEMORY.md `feedback_cert_must_cover_all_paths`).

### 3.5 Option B explicitly deferred

The full in-handler async switch (the handler itself reconciling frames) is deferred unless A's dispatch-on-return latency proves unacceptable on RPi3 `[live]` (it won't matter on QEMU). Recorded here so a later wave has the rationale.

### 3.6 The `[smp-async-preempt]` cert — harness + the cooperative-revert falsifier

**The workload (extends `smp.c`'s `SMP_PREEMPT_TEST` block, gated by a NEW sub-flag `SMP_ASYNC_PREEMPT` so the ②.1a cooperative cert stays selectable):**

- **`smp_async_lowprio_loop` (runs on the secondary B):** a tight, BOUNDED loop that increments a `volatile unsigned long g_async_counter` and **does NOTHING else** — crucially **no `g_resched_pending` poll, no checkpoint** (the opposite of `smp_secondary_preempt_loop`, `smp.c:717-767`, which polls every iteration). It runs as a REAL TCB (via the ②.2a `smp_prod.c` machinery) on the secondary with IRQs UNMASKED so the SGI can be taken asynchronously.
- **`smp_async_highprio_task`:** a higher-priority real TCB the driver readies on CPU 0; its body records `g_async_highprio_ran = 1` and `g_async_observed_counter = g_async_counter` (the value the low-prio loop had reached **at the instant of preemption**).
- **The scenario (driver on CPU 0):** (1) start B on the secondary spinning the no-poll loop; (2) wait until `g_async_counter` is observably advancing (B is genuinely mid-loop); (3) ready the high-prio task into `knl_ready_queue` and `smp_send_reschedule(secondary)`; (4) the SGI fires → `_vec_el1_irq` → handler sets pending → **the §3.2 IRQ-return hook switches B to the high-prio task mid-loop** (NO cooperation); (5) the high-prio task runs, records the mid-loop counter, then `tk_ext_tsk`/blocks so B's low-prio task is re-dispatched; (6) **B's low-prio loop RESUMES** — its counter continues **from where it was** (the driver checks `g_async_counter > g_async_observed_counter` AND the loop completes its remaining iterations), proving register/PC state round-tripped through the async switch.

**PASS (all, watchdog-bounded):** `g_async_highprio_ran == 1`; `g_async_observed_counter` is in (0, CAP) — i.e. the preempt landed **mid-loop**, not before it started or after it finished; the low-prio loop **resumed and finished** (`g_async_counter` reaches its final expected total, proving correct resume); an SGI was actually taken (`smp_sgi_taken(secondary) >= 1`, `smp.c:969-974`). Print `SMP-ASYNC-PREEMPT: PASS`.

**FALSIFIER `-DSMP_NO_ASYNC` (MUST go RED):** compile the §3.2 IRQ-return hook OUT (the handler reverts to ②.1a flag-set-only, `smp.c:258-266`). Everything else byte-identical: B still spins the no-poll loop with IRQs unmasked, the high-prio task is still readied, the SGI is still sent and TAKEN (`smp_sgi_taken >= 1`). But with no IRQ-return switch and **no flag-check in the tight loop**, B is **never preempted** → `g_async_highprio_ran == 0`, the high-prio task never runs on B → watchdog → `SMP-ASYNC-PREEMPT: FAIL`. **This is the load-bearing proof that the mid-loop preempt happens ONLY because of the real context switch on the IRQ-return path** (distinguishing ②.2b from ②.1a's cooperative version). A second sanity assertion: in the FAIL build the SGI is still *taken* (`sgi_taken>=1`) — proving the failure is the missing *switch*, not a missing *delivery*.

**QEMU-testable vs `[live]`:**

| Sub-claim | QEMU `-smp 4` | RPi3 `[live]` |
|---|---|---|
| B is preempted mid-loop; counter resumes correctly (PC/ELR/SPSR/x0..x18 round-trip) | **YES** (full — what `[smp-async-preempt]` proves) | yes |
| `-DSMP_NO_ASYNC` goes RED (no switch → no mid-loop preempt) | **YES** (load-bearing) | yes |
| The IAR-slot non-clobber + EOIR-ordering + frame-nesting on weakly-ordered silicon / real SGI timing | **PARTIAL — QEMU TCG may mask** | **ONLY here** |

---

## 4. Wire the secondary's timer + WAIT path (②.2b-ii)

### 4.1 Why a secondary task FAULTS today (ground the gap precisely)

`tk_dly_tsk`/`tk_slp_tsk` are NOT inherently broken on a secondary — they run entirely under `BEGIN/END_CRITICAL_SECTION` and touch only shared, BKL-protected state. Tracing `tk_dly_tsk_impl` (`time_calls.c:136-153`): it `BEGIN_CRITICAL_SECTION`s (acquires BKL, masks IRQ), sets `CUR_CTXTSK->wspec`, calls `knl_make_wait_reltim` (`wait.c:220-231`) which does `knl_make_non_ready(CUR_CTXTSK)` (`task.c:253-260`), sets `CUR_CTXTSK->state = TS_WAIT`, and `knl_timer_insert_reltim`s the task's `wtmeb` into the **shared** `knl_timer_queue` (`timer.c:51`). Then `END_CRITICAL_SECTION` (`cpu_status.h:54-62`): because `knl_make_non_ready` set `CUR_SCHEDTSK = knl_ready_queue_top(...)` (`task.c:256-257`), now `CUR_CTXTSK != CUR_SCHEDTSK` → it calls `knl_dispatch()` → the secondary switches OFF the blocking task.

**So the BLOCK half already works under the BKL.** The two real gaps are on the **WAKE** half:

1. **No tick on the secondary.** The delay only expires when `knl_timer_handler` (`timer.c:177-231`) walks `knl_timer_queue` and fires the `knl_wait_release_tmout` callback (`wait.c:112+`). `knl_timer_handler` runs from the CNTP PPI 30 IRQ (`tkdev_init.c:138-148`). **Only the BOOT CPU's CNTP timer is programmed** (`timer_init`, `tkdev_init.c:117-133`, runs once inside `knl_t_kernel_main` on CPU 0) and **only the boot CPU's GIC CPU interface is enabled** (`gic_init`, `tkdev_init.c:93-109`). The secondary never gets a tick → a delay queued by a secondary task never expires unless the boot CPU's tick walks the (shared) queue — which it does, BUT:
2. **The cross-CPU WAKE-dispatch gap.** When the boot CPU's `knl_timer_handler` fires `knl_wait_release_tmout` → `knl_wait_release` (`wait.h:156-162`) → `knl_make_non_wait` → `knl_make_ready(tcb)` (`task.c:226-233`), it sets **`CUR_SCHEDTSK = tcb`** — but `CUR_SCHEDTSK` resolves to **the BOOT CPU's** slot (`g_smpcpu[0].schedtsk`), because the boot CPU is the one running the tick. **The woken task belongs on the SECONDARY, not CPU 0.** Nothing tells the secondary to re-dispatch. This is the exact "make `knl_reschedule`/`knl_make_ready` IPI-aware" work the ②.1a doc deferred to ②.2 (`smp-1-ipi-preempt-plan.md:174`, `:273`).

### 4.2 The choice: per-CPU CNTP tick vs boot-CPU-tick-drives-all (PIN ONE)

Two correct options; **②.2b PINS Option T1** (per-CPU CNTP) as the simplest *correct* one that also yields tick-driven RR preemption on the secondary (needed for the §3 async cert's "timer can also preempt" story), with a clear honest reason:

- **Option T1 (PINNED) — each CPU programs its OWN EL1 CNTP timer + enables its OWN GIC CPU interface + unmasks PPI 30.** The CNTP timer (`cntp_tval_el0`/`cntp_ctl_el0`, `tkdev_init.c:126-129`) is **per-CPU banked hardware**; PPI 30 is a per-CPU interrupt. The secondary already enables its CPU interface via `smp_gic_cpuif_init` (`smp.c:242-249`) for the SGI; ②.2b-ii adds (a) `gic_enable_irq(INTNO_TIMER_GIC)` for PPI 30 on the secondary's distributor view — note PPI enable bits in `GICD_ISENABLER0` are per-CPU-banked on GICv2, so this is a per-CPU write that does NOT perturb the boot CPU (§5.3), and (b) a per-CPU `timer_init()`-equivalent that programs the secondary's own `cntp_tval_el0`/`cntp_ctl_el0`. Then the secondary takes its OWN PPI 30 → `_vec_el1_irq` → `knl_intvec[30]` (`timer_irq_handler`, registered globally at `tkdev_init.c:160`) → `knl_timer_handler` under the BKL, walking the SHARED `knl_timer_queue`, charging `CUR_CTXTSK` (which on the secondary is ITS task) and RR-rotating ITS task (`timer.c:187-226`). **This also gives the secondary tick-driven RR preemption "for free" — a second, independent way to trigger the §3 async switch (the tick's `END_CRITICAL_SECTION` calls `knl_dispatch` when RR rotates `CUR_SCHEDTSK`), strengthening the async story.** The reload (`timer_irq_handler`, `tkdev_init.c:138-148`) is already per-CPU-safe (it reads CNTFRQ and writes the local CNTP_TVAL).

  **The cross-CPU wake (gap #2) is still needed even under T1** for the case where CPU 0's tick (or any CPU's tick) releases a task that belongs on another CPU — see §4.3.

- **Option T2 (NOT pinned) — only the boot CPU ticks; it drives all CPUs' time-slicing via SGIs.** The boot CPU's tick, after walking the timer queue, scans `g_smpcpu[].ctxtsk` and `smp_send_reschedule(target)`s any CPU whose schedtsk changed. Simpler hardware (no secondary CNTP), but it (a) makes the secondary's RR depend on the boot CPU's tick latency and (b) duplicates the §4.3 cross-CPU-wake logic into the tick. **Rejected for ②.2b** because T1 is more faithful to "each CPU runs a real, independently-tick-preemptible task" (the END GOAL), and the per-CPU CNTP is genuinely simple (the hardware is already banked; it is 4 instructions, `tkdev_init.c:126-129`). T2 is recorded as the fallback if the secondary's PPI 30 routing proves problematic on RPi3 `[live]` (the BCM2837 path differs — §6).

### 4.3 The cross-CPU wake (gap #2) — make `knl_make_ready` IPI-aware (BKL-safe)

When ANY CPU's tick (or a `tk_wup_tsk` on any CPU) releases a task that should run on a DIFFERENT CPU, that CPU must be told to re-dispatch. **②.2b's minimal, BKL-safe design:** after `knl_make_ready(tcb)` readies a task, if the newly-top task outranks what some OTHER CPU is running (`tcb` should preempt `g_smpcpu[c].ctxtsk`), `smp_send_reschedule(c)` that CPU. The SGI → the §3 async-preempt path → that CPU switches to `tcb`. Concretely, the smallest correct slice for the `[smp-secondary-sleep]` cert: the boot CPU's tick releases the secondary's delayed task; the driver/tick `smp_send_reschedule(secondary)`; the secondary's §3 IRQ-return hook re-dispatches it (its `CUR_SCHEDTSK` is set by a BKL-held `knl_reschedule`-on-behalf, OR the secondary re-runs `knl_reschedule` for itself when it takes the SGI). **The general "scan all CPUs, pick the target, send only if outranked" policy is ②.3 production-scheduler work; ②.2b implements the directed single-target wake the cert needs and ledgers the general policy as deferred** (mirroring how ②.1a proved the SGI mechanism, not the policy, `smp.c:1004-1011`).

**BKL-safety confirmation (§2's claim, discharged):** the entire WAIT/timer path is already inside `BEGIN/END_CRITICAL_SECTION` — `tk_dly_tsk`/`tk_slp_tsk` (`time_calls.c:143-149`, `task_sync.c:216-231`), `knl_timer_handler` (`timer.c:183-228`), `tk_wup_tsk` (`task_sync.c:252-265`). Under ②.2a, `BEGIN_CRITICAL_SECTION` acquires the BKL (`cpu_status.h:50`, `BKL_ACQUIRE()`). So every mutation of `knl_timer_queue`, `knl_ready_queue`, and the TCB wait fields is BKL-serialized across CPUs — **the WAIT path is already BKL-safe; ②.2b adds only the per-CPU tick (T1) and the cross-CPU wake-dispatch (the SGI), both of which respect the lock.** No new unlocked shared state.

### 4.4 The `[smp-secondary-sleep]` cert — harness + falsifier

**Workload (gated `SMP_SECONDARY_SLEEP`, building on `smp_prod.c`):** the secondary's real task B (today parks on `wfe`, `smp_prod.c:97-98`) instead calls `tk_dly_tsk(N_TICKS)`, records `g_sec_slept = 1` before and `g_sec_woke = 1` + `g_sec_wake_time = knl_current_time` after. The driver on CPU 0: brings up the secondary (②.2a path), enables the secondary's per-CPU tick (T1), waits, and asserts B woke. **PASS:** `g_sec_slept == 1 && g_sec_woke == 1` and the wake happened after ≥ N_TICKS of `knl_current_time` advance (proving a real timed block+wake, not a spurious return), within the watchdog. Print `SMP-SECONDARY-SLEEP: PASS`. **FALSIFIER `-DSMP_NO_SEC_TIMER`:** skip enabling the secondary's CNTP PPI (and skip the cross-CPU wake) → the secondary's delay never fires → `g_sec_woke == 0` → watchdog → `SMP-SECONDARY-SLEEP: FAIL`. Proves the secondary timer/wake wiring is load-bearing. **QEMU-testable;** the per-CPU PPI 30 routing + real timer cadence are `[live]`-sharpened on RPi3 (§6).

---

## 5. The determinism + safety boundary (unchanged — state how the secondary timer avoids perturbing N=1)

1. **N=1 / default build BYTE-IDENTICAL.** Every ②.2b symbol is `SMP_SELFTEST`-gated: the §3.2 IRQ-return hook is inside `#ifdef SMP_SELFTEST` in `_vec_el1_irq`; the secondary CNTP enable + cross-CPU wake live in the SMP TUs (`smp.c`/`smp_prod.c`). With no flag, `_vec_el1_irq` is byte-untouched (the existing `:301-351` path), `knl_dispatch`/`.Ldispatch_loop` keep their `#else` plain-global arms (`cpu_support.S:90-92,126-135,151-164`), and `smp.o`/`smp_prod.o` are empty (the Makefile drops `smp_prod.o` from `LINK_OBJS` unless `-DSMP_2TASKS_PROD`, `boot/aarch64/Makefile:242-243`). The `[smp-uniproc-semantics]` guard (default-build `.text` byte-identity, `run_smp2.sh:82-103`) **carries forward unchanged** and must stay green in ②.2b's harness. **The crown constraint (`smp_percpu.h:18-23`): SMP-off macros expand to parenthesized identifiers → gcc compiles byte-identically to the bare globals (proven by objcopy `.text` cmp).**

2. **The production tick/WAIT on the boot CPU at N=1 unchanged.** At N=1 there is no secondary; the boot CPU's `timer_init`/`gic_init` (`tkdev_init.c:117-162`) run exactly as today, `knl_timer_handler` (`timer.c:177-231`) walks the queue exactly as today (CUR_* resolve to `g_smpcpu[0]` under SMP, which has exactly one live entry, or to the plain globals with SMP off). The §4.3 cross-CPU wake only fires when a secondary actually runs (`SMP_MAX_CPUS` slots, all but slot 0 dormant at N=1).

3. **How the secondary's timer enable avoids perturbing the boot CPU's CNTP/GIC at N=1 (the §5.3 obligation, discharged).** Under Option T1, the secondary writes ONLY per-CPU-banked hardware: `cntp_tval_el0`/`cntp_ctl_el0` (per-core system registers — writing CPU 1's cannot touch CPU 0's), `GICC_PMR`/`GICC_CTLR` (the per-CPU-banked CPU interface, already established harmless by ②.1a, `smp.c:238-249`), and the PPI 30 enable bit in `GICD_ISENABLER0` (**PPI/SGI enable bits are per-CPU-banked on GICv2** — each CPU sees its own banked copy of `GICD_ISENABLER0`, so enabling CPU 1's PPI 30 does not enable/alter CPU 0's). The secondary does **NOT** touch the shared distributor `GICD_CTLR` (only the boot CPU's `gic_init` enables it, `tkdev_init.c:99`; ②.1a's `smp_gic_selftest_setup` re-enables it idempotently for the self-test window, `smp.c:277`). **At N=1 none of this runs at all.** The auditor must independently confirm the GICv2 PPI-banking claim against the running QEMU (the "GIC subtlety the audit confirms against the running QEMU, not the spec" rule, `smp-1-ipi-preempt-plan.md:110`).

4. **The BKL still serialises kernel state (§5.4).** The §3 async switch happens from IRQ context but reuses `knl_dispatch`, which runs with IRQ/FIQ masked (`cpu_support.S:103`) and switches per-CPU `g_smpcpu[me]` state only. The §4 WAIT/tick path is wholly inside `BEGIN/END_CRITICAL_SECTION` → BKL-held (§4.3). The SGI handler still only sets a per-CPU flag (`g_resched_pending[me]`, `smp.c:263`, one writer/one reader → `dmb` suffices, no BKL, `smp.c:211-214`). **One new re-entrancy to audit:** a timer IRQ that fires on a CPU already holding the BKL (mid-syscall) must not self-deadlock — the BKL is recursive (`bkl_acquire` owner-check, `smp.c:404-408`), so a nested acquire bumps depth without re-locking. But the §3 async switch from THAT nested IRQ must NOT dispatch while the BKL depth > the entry depth (you cannot switch away from a task mid-critical-section). **②.2b's IRQ-return condition must therefore ALSO gate on "not currently holding the BKL from a deeper frame" — i.e. only async-dispatch when the interrupted context was NOT inside a kernel critical section.** This is implicit in the `!CUR_DISPATCH_DISABLED && !knl_isTaskIndependent()` clauses for the common case, but the auditor must falsify the "preempt while BKL-held mid-syscall" case explicitly (a `[smp-no-deadlock]`-style nested-IRQ falsifier). **This is the single most likely place ②.2b self-deadlocks; flag it for independent audit.**

5. **The mind is untouched** (§2.2). No mind math, weights, engram, or `gl_merge` touched. `[smp-one-mind]` is ②.2c (§7).

---

## 6. Honesty (lead with the limits)

1. **②.2b is the aarch64 IRQ-path C-ABI trap, concentrated.** The async switch from interrupt context — reconciling the 160-byte `save_caller_regs` frame (`cpu_support.S:244-256`) with `knl_dispatch`'s 112-byte frame (`:106-115`) and threading ELR_EL1/SPSR_EL1 through the resume (§3.3) — is the single hardest, most fault-prone piece in the repo. The predicted failure mode is the recurring "garbage-PC crash" (pc==addr==random) from a mis-nested frame, or a silently-undeactivated SGI from a clobbered IAR slot. The auditor independently re-derives the §0.2 trap analysis against the running QEMU.

2. **The barrier/IRQ-timing teeth are only fully `[live]` on RPi3 — and QEMU may MASK them.** QEMU TCG models memory strongly and may hide a missing `dsb ish`/SMPEN race; it also delivers SGIs and ticks with more forgiving timing than silicon. A QEMU `[smp-async-preempt]`/`[smp-secondary-sleep]` green proves the **switch is correctly plumbed and load-bearing** (mid-loop preempt + resume; secondary block+wake); it does **NOT** prove the barrier discipline or real interrupt latency on weakly-ordered hardware. **A QEMU green is NOT a hardware green.** Additionally, **RPi3 is NOT GICv2** — it uses the BCM2837 ARM Local Interrupt Controller (per-core mailbox IPIs + a different timer-INTCTL path, `tkdev_init.c:38-56`); the `GICD_SGIR` send (`smp.c:221-236`) and the per-CPU PPI 30 enable are QEMU-virt-GICv2-specific. The RPi3 `[live]` port of both ②.2b certs needs the BCM2837 mailbox-IPI + per-core timer-INTCTL paths (a `BOARD_RPI3` `#ifdef`, mirroring the existing fork, `tkdev_init.c:29-111`) — a deferred `[live]` follow-up.

3. **②.2b implements the MECHANISM + the directed cert scenario, not the general SMP scheduling policy.** The cross-CPU wake (§4.3) is a directed single-target SGI for the cert, not the general "scan all CPUs, pick the lowest-priority target, send only if outranked" policy (that is ②.3). `knl_taskindp` is still a shared global (§3.4) — ②.2b either per-CPU-izes it or ledgers it as a sharpening. These are honest, bounded scopes.

4. **This plan makes ②.2b implementation-ready; it does not start it.** Read-only on code. The implementation is a separate impl→audit→integrate cycle on this same base `ed63e951`.

---

## 7. Sequencing + honest deferral

Each step: a falsifiable cert + a falsifier that MUST go RED, on the **explicit-hash base `ed63e951`**, via a **separate impl→audit cycle** (implementer ≠ auditor ≠ commander).

**②.2b-i — true async register-context preempt on the SGI path + `[smp-async-preempt]`. (THE CORE; the §0.2 trap wave.)**
- The §3.2 IRQ-return hook in `_vec_el1_irq` (`cpu_support.S:301-351`): after EOIR, before `restore_caller_regs`, Option-A dispatch reusing `knl_dispatch` (§3.2), with the ELR/SPSR resume reconciliation (§3.3) and the per-CPU `knl_taskindp`/BKL-depth guards (§3.4, §5.4).
- **Cert `[smp-async-preempt]`:** a no-poll tight-loop secondary task is preempted MID-LOOP by an SGI and RESUMES correctly (counter continues) → `SMP-ASYNC-PREEMPT: PASS`. **Falsifier `-DSMP_NO_ASYNC`** (revert to ②.1a flag-set-only, no IRQ-return switch) → no mid-loop preempt → FAIL (with `sgi_taken>=1` proving the failure is the missing switch, not missing delivery). New `tests/aarch64/run_smp3.sh` + `run-smp3`/`smp3-test` Makefile targets, modeled on `run_smp2.sh` / `smp2-test` (`boot/aarch64/Makefile:485-535`). **The auditor independently re-derives the EOIR-ordering, the IAR-slot non-clobber, the frame-nesting, and runs the BKL-held-nested-IRQ falsifier (§5.4).**

**②.2b-ii — secondary timer + WAIT wiring + `[smp-secondary-sleep]`.**
- Option T1 (§4.2): the secondary programs its own CNTP (`cntp_tval_el0`/`cntp_ctl_el0`) + enables its banked GIC CPU interface + PPI 30; the cross-CPU wake (§4.3) makes a released task re-dispatch on its CPU via the SGI (reusing ②.2b-i's async switch).
- **Cert `[smp-secondary-sleep]`:** a real secondary task `tk_dly_tsk`s and WAKES → `SMP-SECONDARY-SLEEP: PASS`. **Falsifier `-DSMP_NO_SEC_TIMER`** (no secondary tick/wake) → never wakes → FAIL.

**DEFERRED past ②.2b (explicit):**
- **②.2c — the `[smp-one-mind]` CROWN cert** (the real bare-metal `r_forward`, `r3_incontext.c`, byte-identical uniproc vs SMP under N=2+ actually scheduling; FNV-1a logit-hash equality; the `-DSMP_ONEMIND_RACE` + `-DSMP_NO_RQLOCK` falsifiers). The gate that says "② did not split the mind." ②.2b does NOT touch it.
- **②.3 — finer locks** (split BKL → `g_rqlock`/`g_memlock`/object locks) + per-CPU run-queues + migration/load-balancing + the **general** cross-CPU reschedule policy (§4.3) + per-CPU `knl_taskindp` (§3.4). Re-run every cert after each split.
- **Option B** (full in-handler async switch, §3.5) unless A's latency proves unacceptable on `[live]`.
- **Hosted-port SMP** (threads-as-CPUs; the dispatch-on-signal-return analogue of §3.2) — separate lift. **x86_64/rl78 bare-metal SMP** — ②.2b is aarch64-only.
- **RPi3 `[live]`** — the BCM2837 mailbox-IPI + per-core timer-INTCTL paths for both certs (§6 #2); the real barrier/timing teeth on hardware.

**②.2b keeps the production kernel BYTE-IDENTICAL at N=1 and the mind UNTOUCHED** (§5). It is the wave that makes a secondary genuinely preemptible mid-computation and able to sleep/wake — the last mechanism gap before the ②.2c crown.

---

## Appendix — grounding (file:line, all on trunk `ed63e951`)

**The ②.2a base ②.2b extends:**
- The production dispatcher (per-CPU under SMP; the save/restore ②.2b reuses): `cpu_support.S:101-186` — `knl_dispatch` save (`:103-135`, 112-byte frame, `str x1,[x0,#TCB_SSP]` `:123`), `.Ldispatch_loop` restore (`:140-180`), `.Lidle` (`:182-186`); `LD_PERCPU_BASE` (`:67-75`); `knl_task_entry_trampoline` (`:194-211`); `smp_prod_enter_dispatch` → `b .Ldispatch_loop` (`:500-502`).
- `TCB_SSP == 192` (`TCB_tskctxb`+`CTXB_ssp`): `arch/common/include/lp64/offset.h:80,86,88`; `TCB_task == 40` (`:80`).
- The IRQ vector (where §3.2 hooks): `_vec_el1_irq` `cpu_support.S:301-351` — `save_caller_regs` (160B, `:244-256,302`), IAR slot (`:307,324,340-341`), `knl_intvec[INTID]` blr (`:331-336`), EOIR (`:345-347`), `restore_caller_regs`+`eret` (`:350-351`); `gicc_base_ptr` (`:424-425`).
- The CUR_* accessors + critical-section macros: `arch/aarch64/include/cpu_status.h:24-26` (CUR_CTXTSK/SCHEDTSK/DISPATCH_DISABLED), `:50` (BKL_ACQUIRE), `:54-62` (`END_CRITICAL_SECTION` 4-clause dispatch test — the condition §3.1 Option A mirrors), `:85` (`in_indp`); the x86/linux fallback `task.h:193-209`.
- `knl_reschedule` (CUR_*-based): `include/kernel/tkernel/task.h:268-277`; `knl_dispatch_request()` no-op `cpu_status.h:104`.
- The per-CPU SMP block + asm offsets: `smp.c:310-356` (struct `smp_cpu`, `dispatch_disabled@64`, `SMPCPU_SIZE 72`, `_Static_assert`s `:344-349`); the typed view `smp_percpu.h:53-72`; asm mirror `cpu_support.S:61-65`; `smp_this_cpu` (MPIDR Aff0) `smp.c:356`; `smp_cur_tcb_load` `cpu_support.S:469-476`.
- The BKL (recursive): `smp.c:374-424` (`bkl_acquire` owner-check `:404-408`, recursion `:406`).
- The ②.1a SGI path ②.2b makes context-switching: `smp_send_reschedule` (`smp.c:221-236`, `-DSMP_NO_IPI` no-op `:223-227`), `smp_gic_cpuif_init` (`:242-249`), `smp_resched_sgi_handler` (sets flag only — the thing ②.2b's IRQ-return path replaces) (`:258-266`), `g_resched_pending[]`/`g_sgi_taken[]` (`:214,216`), the cooperative-at-checkpoint loop (the thing ②.2b's async path replaces) `smp.c:717-774`, the explicit "true async preempt DEFERRED to ②.2" `smp.c:689-691`.
- ②.2a real-task driver (the secondary running a REAL TCB; the wfe-park gap ②.2b-ii closes): `smp_prod.c:68-99` (`smp_prod_task_b`, the wfe park + honest-scope comment `:86-98`), `:115-185` (`smp_prod_test_run`), `is_real_tcb` `:103-111`; the secondary's `SMP_2TASKS_PROD` dispatch path `smp.c:796-816`.

**The production WAIT + timer path ②.2b wires for the secondary:**
- `tk_dly_tsk_impl` (block half, already BKL-safe): `kernel/common/time_calls.c:136-153` (`BEGIN_CRITICAL` `:143`, `knl_make_wait_reltim` `:147`, `END_CRITICAL` `:149`).
- `tk_slp_tsk_impl`: `kernel/common/task_sync.c:209-234` (`knl_make_wait` `:226`); `tk_wup_tsk_impl` (`:241-268`, `knl_wait_release_ok` `:258`).
- `knl_make_wait`/`knl_make_wait_reltim` (→ shared `knl_timer_queue`): `kernel/common/wait.c:192-216,220-231` (`knl_make_non_ready` `:196,224`, `knl_timer_insert(_reltim)` `:203,231`); `knl_wait_release`/`knl_make_non_wait` (→ `knl_make_ready`): `include/kernel/tkernel/wait.h:124-150,156-162`.
- `knl_make_ready` (the cross-CPU wake gap — sets `CUR_SCHEDTSK` of the WAKING CPU): `kernel/common/task.c:226-233` (`CUR_SCHEDTSK = tcb` `:230`); `knl_make_non_ready` `:253-260`.
- `knl_timer_handler` (the tick — reads/charges `CUR_CTXTSK`, RR-rotates, walks shared `knl_timer_queue`): `kernel/common/timer.c:177-231` (`BEGIN_CRITICAL` `:183`, time charge `:187-194`, queue walk `:199-216`, RR `:219-226`, `END_CRITICAL` `:228`); `knl_current_time`/`knl_timer_queue` defs `:45,51`.
- The aarch64 CNTP PPI 30 setup (boot CPU only today — the per-CPU hardware §4.2 enables on the secondary): `tkdev_init.c:117-133` (`timer_init`, `cntp_tval_el0`/`cntp_ctl_el0` `:126-129`), `:138-148` (`timer_irq_handler` reload + `knl_timer_handler_startup`), `:154-162` (`knl_tkdev_initialize`: `gic_init` `:159`, `knl_define_inthdr(INTNO_TIMER_GIC,...)` `:160`, `gic_enable_irq` `:161`, `timer_init` `:162`); `gic_init` (distributor + boot CPU interface only) `:93-109`; `INTNO_TIMER_GIC 30` `tkdev_conf.h:42`, `TIMER_HZ 100` `:10`, `CFN_TIMER_PERIOD 10` `utk_config_depend.h:27`.
- `knl_timer_handler_startup` (asm, `knl_taskindp++/--`): `cpu_support.S:217-237`.

**The task-independent / dispatch-disable state the §3.1 condition reads:**
- `knl_taskindp` (STILL A GLOBAL at `ed63e951` — §3.4): `arch/aarch64/include/cpu_insn.h:64`, `knl_isTaskIndependent` `:66-69`, init `cpu_init.c:17`.
- `knl_dispatch_disabled` (per-CPU under SMP via `CUR_DISPATCH_DISABLED`): global def `kernel/common/task.c:55`; per-CPU slot `smp.c:334`, `smp_percpu.h:67`; `CHECK_DISPATCH`/`in_ddsp` `include/kernel/tkernel/check.h:236-240`, `cpu_status.h:84`.

**The cert harness pattern + gating (model `run_smp3.sh` on these):**
- ②.2a harness: `tests/aarch64/run_smp2.sh` (build `-DSMP_SELFTEST -DSMP_2TASKS_PROD` `:62`, grep `cpu1 entered PRODUCTION dispatcher`/`SMP-2TASKS-PROD: PASS`/`B ran=1`/`Initial task started` `:71-78`, `[smp-uniproc-semantics]` `.text` byte-identity guard `:82-103`); ②.1a harness `run_smp1.sh` (the `-DSMP_NO_IPI` falsifier pattern).
- The cert verdict-print pattern: `arch/aarch64/usermain.c:224-270` (`SMP_2TASKS_PROD`, `smp_prod_test_run` + evidence print + `SMP-2TASKS-PROD: PASS/FAIL`); `boot/aarch64/main.c:381-418` (`SMP_PREEMPT_TEST`, `smp_preempt_test_run` + `SMP-PREEMPT: PASS/FAIL`).
- Makefile SMP targets + gating: `boot/aarch64/Makefile:242-243` (drop `smp_prod.o` unless `-DSMP_2TASKS_PROD` → default byte-identical), `:382` (`QEMU_SMP_FLAGS -smp 4`), `:474-535` (`run-smp0/1/2`, `smp0/1/2-test`, the falsifier targets).

---

**Note:** This plan is a DESIGN PLAN on trunk `ed63e951` (②.0/②.1a/②.1b/②.2a all merged). ②.2b is the most C-ABI-fault-prone wave in the repo (an asynchronous register-context switch from interrupt context, threaded through the aarch64 IRQ vector — the repo's recurring failure mode) and is **awaiting mk_pino's go-ahead**, via a **separate impl→audit cycle** (implementer ≠ auditor ≠ commander; the development METHOD is the project's immune system). This plan makes ②.2b READY and de-risked — it does **not** start it. The smallest real first slice is **②.2b-i** (the §3.2 IRQ-return async switch + `[smp-async-preempt]` PASS with the `-DSMP_NO_ASYNC` cooperative-revert falsifier going RED); **②.2b-ii** wires the secondary timer/WAIT (`[smp-secondary-sleep]`). The crown `[smp-one-mind]` is **②.2c** — the gate that says ②.2 did not split the mind.
