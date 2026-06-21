# ②.1 — cross-CPU preemption via GIC SGI IPIs: implementation plan (cert-first)

**Status: DESIGN PLAN** by an automated design-harden on trunk `9db8a998` (②.0 merged + audited MERGEABLE). Read-only on code; no implementation in this wave. This plan makes ②.1 **READY and de-risked** on top of the shipped ②.0 BKL slice — it does **not** start it. ②.1's implementation awaits **commander review** + a **separate impl→audit cycle** (implementer ≠ auditor ≠ commander; the development METHOD is the project's immune system).

> Base confirmed: in the shared checkout `git rev-parse --short HEAD` == `9db8a998` (`git log`: `9db8a998 backlog: ②.0 SHIPPED+audited … ②.1 IPI next` → `78c5d222 ②.0: full-SMP slice — 2 aarch64 CPUs run the dispatcher under one Big Kernel Lock`). All file:line grounding below is against the shared checkout at `9db8a998`.

---

## 0. Where ②.0 left us, and the one thing ②.1 adds (lead with the boundary)

②.0 (`arch/aarch64/smp.c`, on trunk) proved the **smallest real SMP slice**: two aarch64 CPUs each run their OWN per-CPU dispatcher (`smp_dispatch_run`, `smp.c:453-494`) over their OWN per-CPU scheduler state (`g_smpcpu[]`, `smp.c:149-168` with `ctxtsk`/`schedtsk`/`exec_count`/`cpu_id`/`live`), serialized by ONE recursive Big Kernel Lock (`bkl_acquire`/`bkl_release`, `smp.c:215-239`). The two CPUs **cooperatively PULL** distinct tasks from one shared ready list (`smp_ready_pull`, `smp.c:281-295`) — there is **no mechanism for one CPU to force another to reschedule**. The honest deferral is stated in `smp.c:39-43`:

> *"IPIs / cross-CPU preemption (GICD_SGIR + reschedule SGI) → ②.1. ②.0 uses cooperative pull from the shared ready list; a CPU re-checks the ready list when its current task yields/blocks. No CPU forces a reschedule on another (§4.3)."*

**What ②.1 adds — and ONLY this:** the **GIC SGI (Software-Generated Interrupt) path that does not exist today**, so CPU A can SEND an interrupt to CPU B that forces B to re-run its per-CPU dispatcher and pick up a now-higher-priority task. This is **real, genuinely new interrupt-controller work** — the GIC has zero SGI plumbing on trunk (proven §2.1). It is the first hardware-facing piece of ②: ②.0 was pure shared-memory cooperation; ②.1 reaches into the GICv2 distributor to deliver a cross-CPU interrupt.

**The boundary ②.1 must hold (unchanged from ②.0, restated as a hard contract):**

1. **The shipped uniprocessor kernel stays BYTE-IDENTICAL.** Every ②.1 symbol is compiled ONLY under `-DSMP_SELFTEST` (`smp.c:73`, `cpu_support.S:382`, `start.S:282`). With no flag, smp.c is an empty object and the production dispatcher `.Ldispatch_loop` (`cpu_support.S:90-124`) + `task.c` globals (`knl_ctxtsk`/`knl_schedtsk`, `task.c:62-63`) are untouched. The default `make` build carries NONE of ②.1.
2. **②.1 stays in the ②.0 self-test SANDBOX.** It does NOT convert the ~166 production `knl_ctxtsk`/`knl_schedtsk` reader sites to per-CPU — that is **②.2**, explicitly deferred (§7). ②.1 sends its reschedule SGI to the **②.0 self-test dispatcher** (`smp_dispatch_run`) over the **②.0 self-test tasks** (`struct smp_task`, `smp.c:257-262`). The cert proves the IPI MECHANISM is load-bearing; it does not yet preempt the real shell.
3. **The production IRQ path must not be perturbed when SMP is disabled, and must not be corrupted when it IS.** The new SGI receive code threads into the EXISTING `_vec_el1_irq` (`cpu_support.S:234-284`) — the repo's recurring AArch64-IRQ-path C-ABI pitfall ("when input/timing doesn't work on aarch64, suspect the IRQ vector before the device") means the SGI handler is the highest-risk surface in ②.1, and it is gated + cert-falsified accordingly.
4. **The one-mind crown is insulated.** ②.1 touches no mind math. The `[smp-one-mind]` byte-identity crown cert remains **②.2** (it needs the production scheduler under SMP, which ②.1 does not deliver). ②.1's obligation to the crown is purely negative: **do not regress the default build's byte-identity** (Boundary 1).

---

## 1. The smallest real ②.1a slice (lead with what ships first)

**②.1a is the whole new mechanism in one falsifiable step: GIC SGI send + receive, driving one cross-CPU preemption, proven by `[smp-cross-preempt]` on QEMU `-smp`.** Concretely, ②.1a adds exactly five things, all gated behind `SMP_SELFTEST`:

1. **`GICD_SGIR` (offset 0xF00) defined** in `tkdev_conf.h` (§2.1) — the register that sends an SGI.
2. **`smp_send_reschedule(cpu)`** in `smp.c` — writes `GICD_SGIR` to deliver SGI id 0 ("reschedule") to a target CPU (§2.2).
3. **Per-CPU GIC CPU-interface init on the secondary** — the secondary enables ITS OWN `GICC_CTLR`/`GICC_PMR` so it can RECEIVE the SGI (today only the boot CPU's CPU interface is enabled, by `gic_init`, `tkdev_init.c:104-106`) (§2.3).
4. **An SGI handler** registered in `knl_intvec[0]` that, running on the target CPU, sets a per-CPU "resched pending" flag the dispatcher re-checks (§2.4).
5. **The `[smp-cross-preempt]` cert** (§4): CPU B runs a LOW-prio spin task; CPU A readies a HIGH-prio task targeted at B and calls `smp_send_reschedule(B)`; B takes the SGI and provably switches to the high-prio task within a bounded watchdog; print `SMP-PREEMPT: PASS`. Falsifier `-DSMP_NO_IPI`: disable the send → B never preempts → `SMP-PREEMPT: FAIL`.

**②.1a deliberately keeps N=2** (boot CPU + the ONE secondary ②.0 already brings up, `SMP_MAX_CPUS 2`, `smp.c:147`). N=4 (`mc2_smp_release_n`) and idle-CPU-wakeup-via-SGI are **②.1b** (§5). This keeps ②.1a to exactly "the IPI exists and is provably load-bearing," reusing ②.0's bringup verbatim.

Everything below grounds each of these five in file:line.

---

## 2. The GIC SGI/IPI path — the core new work

### 2.1 The gap today (grounded: the GIC has NO SGI plumbing)

- **GIC init does distributor enable + timer PPI + CPU-interface enable + priority mask, and nothing else.** `gic_init` (`tkdev_init.c:93-109`): writes `GICD_CTLR=1` (`:99`), enables timer PPI id 30 via `gic_enable_irq` (`:102`, which writes `GICD_ISENABLER`, `:84`), sets `GICC_PMR=0xFF` (`:105`) and `GICC_CTLR=1` (`:106`). **No SGI send, no per-CPU re-init.**
- **`GICD_SGIR` is NOT defined.** `tkdev_conf.h:32-38` defines `GICD_CTLR 0x000`, `GICD_ISENABLER 0x100`, `GICC_CTLR 0x000`, `GICC_PMR 0x004`, `GICC_IAR 0x00C`, `GICC_EOIR 0x010` — **no `GICD_SGIR` (0xF00)**. Grep confirms the ONLY `SGIR`/`SGI`/`IPI` token anywhere under `arch/aarch64/` is the deferral comment in `smp.c:40` (`grep -rni "sgir\|sgi\b\|ipi" arch/aarch64/` → one hit, the comment). Bases (QEMU virt): `GICD_BASE 0x08000000`, `GICC_BASE 0x08010000` (`tkdev_conf.h:28-29`).
- **The IRQ vector can RECEIVE an SGI but nothing sends one and no handler is registered.** `_vec_el1_irq` (`cpu_support.S:234-284`) reads `GICC_IAR` (`:256`), masks the INTID to 10 bits (`and w2, w1, #0x3FF`, `:258`), drops spurious ≥512 (`:259-261`), indexes `knl_intvec[INTID]` (`:264-269`, `blr x3`), and EOIRs (`:280`, `str w1,[x0,#0x10]`). SGI INTIDs are 0-15 and WOULD arrive through `GICC_IAR` — but `knl_intvec[0..15]` are all NULL (`cpu_init.c:14,28-30` zeroes the whole table; only `knl_intvec[30]` is set, `tkdev_init.c:160`), so an SGI today hits the `cbz x3, .Lirq_no_dispatch` (`:268`) and is silently EOIRed with no effect.

### 2.2 SEND: `GICD_SGIR` + `smp_send_reschedule(cpu)`

Add to `tkdev_conf.h` (next to the existing GICD offsets, `:32-38`):

```c
#define GICD_SGIR       0xF00   /* GICv2 Software-Generated Interrupt Register */
```

The GICv2 `GICD_SGIR` write format (ARM GICv2 architecture spec, the standard layout the QEMU virt GICv2 implements):

```
bits [25:24] TargetListFilter  (0b00 = use TargetList in [23:16])
bits [23:16] CPUTargetList     (bitmask of target CPU interfaces; bit n → CPU n)
bit  [15]    NSATT             (group; 0 for our single-group setup)
bits [3:0]   SGIINTID          (the SGI id 0..15)
```

Add to `smp.c` (gated under `SMP_SELFTEST`, using its existing blind-MMIO idiom; carry `GICD_BASE`/`GICD_SGIR` as local `#define`s to stay header-light like the rest of the file, e.g. `smp.c:132-136` already locally `#define`s the UART base — or `#include "tkdev_conf.h"`):

```c
#define SMP_RESCHED_SGI   0U          /* SGI INTID 0 = "reschedule" */

/* Send the reschedule SGI to ONE target CPU (cpu = MPIDR Aff0 / CPU-interface
 * index). GICv2 SGIR: TargetListFilter=0 (use list) | (1<<cpu)<<16 | sgi_id. */
void smp_send_reschedule(int cpu)
{
    unsigned int val = ((1u << (unsigned)cpu) << 16) | (SMP_RESCHED_SGI & 0xF);
    /* Ensure the data the target will observe (the readied high-prio task in
     * the shared ready list) is globally visible BEFORE the SGI is delivered. */
    __asm__ volatile("dsb ish" ::: "memory");
    *(volatile unsigned int *)(GICD_BASE + GICD_SGIR) = val;
    __asm__ volatile("dsb ish" ::: "memory");   /* push the MMIO write out */
}
```

The `dsb ish` BEFORE the SGIR write is load-bearing for correctness on weakly-ordered silicon: the target CPU's handler will inspect the shared ready list (§2.4), and the readied task must be visible to it before the interrupt is delivered. (On QEMU TCG this barrier may be masked — see §6 honesty.)

`#ifdef SMP_NO_IPI` (the falsifier, §4) compiles `smp_send_reschedule` to a no-op body (`(void)cpu;`) so the send is disabled while everything else is identical.

### 2.3 RECEIVE part 1: per-CPU GIC CPU-interface init on the secondary

The GICv2 CPU interface (`GICC_CTLR`/`GICC_PMR`) is **per-CPU banked hardware** — each core sees its own. Today `gic_init` runs **only on the boot CPU** (called from `knl_tkdev_initialize` → `InitModule(tkdev)` during `knl_t_kernel_main`, which the boot CPU runs; `tkdev_init.c:159`). The ②.0 secondary, released into `_secondary_dispatch_entry` (`start.S:284`) BEFORE T-Kernel boots, **never enables its own CPU interface** — so it cannot receive ANY interrupt, SGI included.

②.1 adds a minimal per-CPU CPU-interface enable that the secondary calls once on entry, BEFORE it starts the dispatcher loop. Add to `smp.c` (gated):

```c
/* Per-CPU GIC CPU-interface enable for a secondary. The DISTRIBUTOR
 * (GICD_CTLR, the shared block) is already enabled by the boot CPU's
 * gic_init (tkdev_init.c:99) — we must NOT touch it (Boundary: do not
 * perturb the boot CPU's existing GIC setup). We enable ONLY this CPU's
 * OWN banked CPU interface (GICC_PMR priority mask + GICC_CTLR enable),
 * exactly mirroring tkdev_init.c:105-106 but for the local core. */
void smp_gic_cpuif_init(void)
{
    *(volatile unsigned int *)(GICC_BASE + GICC_PMR)  = 0xFFu;  /* allow all  */
    *(volatile unsigned int *)(GICC_BASE + GICC_CTLR) = 1u;     /* enable     */
    __asm__ volatile("dsb ish; isb" ::: "memory");
}
```

**SGI enablement detail (must cover this path):** in GICv2 the SGI INTIDs 0-15 are **always enabled at the distributor** (the `GICD_ISENABLER0` SGI bits are typically RAO/fixed-enabled on GICv2) — so no extra `GICD_ISENABLER` write is needed for SGI id 0, unlike the timer PPI which `gic_enable_irq` must explicitly unmask (`tkdev_init.c:84`). The plan must VERIFY this on the actual QEMU virt GICv2 during impl (it is the standard GICv2 behavior; if QEMU requires it, add `gic_enable_irq(SMP_RESCHED_SGI)` once on the distributor, which is harmless — id 0's bit). The implementer must check `GICC_CTLR=1` actually routes SGIs (it does on GICv2 with a single group). **This is exactly the kind of GIC subtlety the audit must independently confirm against the running QEMU, not just the spec.**

Wire `smp_gic_cpuif_init()` into the secondary's path. Two clean options; prefer the C one for safety (the repo's IRQ-path C-ABI history says minimize new asm in the interrupt path):

- **(Preferred) Call it from C at the top of `smp_dispatch_run`** (`smp.c:453`), right after the `me` bounds check (`smp.c:455-458`) and before `g_smpcpu[me].live = 1` (`smp.c:460`). Guard it to run the CPU-interface init once per CPU. **Note:** the ②.0 self-test runs BEFORE `knl_t_kernel_main` (`main.c:303-359` is before `:361`), so at self-test time NEITHER CPU's interface is enabled and the distributor is not yet enabled (today `gic_init`'s `GICD_CTLR=1` also happens only at T-Kernel boot). Therefore BOTH CPUs need `smp_gic_cpuif_init()`, AND the distributor must be enabled once before the self-test — see §2.5 init ordering.
- (Alternative) Call it from asm in `_secondary_disp_el1_setup` (`start.S:319-349`) before `b smp_dispatch_loop` (`:349`) — more asm in the bringup path; avoid unless the C call ordering proves awkward.

### 2.4 RECEIVE part 2: the SGI handler + per-CPU resched-pending

Register an SGI handler for INTID 0 in `knl_intvec[0]`. The handler runs on the TARGET CPU in IRQ context (reached via `_vec_el1_irq` → `knl_intvec[INTID]` `blr`, `cpu_support.S:264-269`). It must do the **minimum**: record that THIS CPU should re-evaluate its schedulable task, then return; the EOIR is done by the existing vector tail (`cpu_support.S:280`).

Add to `smp.c` (gated):

```c
/* Per-CPU "a reschedule was requested on me" flag (BSS, VA==PA). The SGI
 * handler sets it; the dispatcher loop re-checks it. Indexed by MPIDR Aff0,
 * sized SMP_MAX_CPUS like g_smpcpu[]. */
volatile unsigned int g_resched_pending[SMP_MAX_CPUS];

/* SGI handler — runs on the TARGET CPU in IRQ context via knl_intvec[0]
 * (cpu_support.S:264-269). Keep it ABI-minimal (the IRQ-path C-ABI pitfall):
 * a plain void(void) the existing vector calls with blr and EOIRs after. We
 * only set a flag; the actual re-dispatch happens on the dispatcher loop's
 * next check (no context switch inside the IRQ handler in ②.1a). */
void smp_resched_sgi_handler(void)
{
    unsigned long me = smp_mpidr_aff0();
    if (me < SMP_MAX_CPUS)
        g_resched_pending[me] = 1u;
    __asm__ volatile("dmb ish" ::: "memory");
}
```

The handler's signature MUST match what `_vec_el1_irq` calls: a `void(*)(void)` invoked with `blr x3` (`cpu_support.S:269`) after `save_caller_regs` (`:235`), with the GIC IAR value stashed on the stack (`:240,256-257`) and EOIRed on return (`:273-280`). `smp_resched_sgi_handler` is a leaf C function clobbering only call-clobbered regs — ABI-clean. **The IRQ-path C-ABI trap to watch (repo history):** do NOT let the handler make a call that needs the stashed IAR, and do NOT let it touch `sp` below the reserved 16-byte slot the vector relies on (`cpu_support.S:240,274`). A flag-set leaf is the safest possible shape.

Register it during the self-test driver (before releasing the secondary), via the same `knl_define_inthdr` the timer uses (`tkdev_init.c:160`):

```c
extern void knl_define_inthdr(int vecno, void (*)(void));  /* cpu_insn.h:53 */
knl_define_inthdr(SMP_RESCHED_SGI, smp_resched_sgi_handler);   /* knl_intvec[0] */
```

`knl_define_inthdr` bounds-checks `vecno < N_INTVEC` (512, `tkdev_conf.h:49`) and writes `knl_intvec[0]` (`cpu_insn.h:53-58`). At ②.0/②.1 self-test time `knl_intvec` is raw BSS (all NULL from `start.S` BSS clear, `:106-115`; `knl_cpu_initialize`'s zeroing, `cpu_init.c:28-30`, runs INSIDE `knl_t_kernel_main`, which is AFTER the self-test) — so registering `knl_intvec[0]` directly is fine for the self-test. This ordering is the trickiest integration point — see §2.5.

**The dispatcher re-check.** `smp_dispatch_run` (`smp.c:464-489`) currently loops: pull a task, run it to completion (`smp_run_task`, `smp.c:443-450`), repeat until drained. For ②.1a's preemption cert, B's LOW-prio task must be **interruptible** — i.e. it must periodically check `g_resched_pending[me]` and, when set, re-enter the pull/select under the BKL so the now-readied HIGH-prio task is chosen. The minimal sandbox change: make the self-test LOW-prio spin task a bounded loop that checks the pending flag each iteration (§3 defines the exact task shapes). This keeps ②.1a's preemption "cooperative-at-a-checkpoint" — a **true** asynchronous mid-instruction preempt (saving B's full register context inside the SGI handler and switching stacks) is the production context-switch work DEFERRED to ②.2 (§7). ②.1a proves: **the SGI is delivered, the handler runs on B, B observes it and re-selects to the high-prio task within bounded time.** That is the load-bearing IPI claim.

### 2.5 Init ordering — the integration linchpin (state it explicitly)

The self-test runs before `knl_t_kernel_main` (`main.c:303-359` then `:361`), so NONE of the GIC state `gic_init` normally sets up exists yet at self-test time. ②.1a's driver must, on the BOOT CPU, in this order, BEFORE releasing the secondary:

1. **Enable the distributor once:** `*(volatile unsigned int*)(GICD_BASE+GICD_CTLR) = 1;` (mirrors `tkdev_init.c:99`). Shared block; idempotent with the later `gic_init` (which re-enables it harmlessly during T-Kernel boot).
2. **Register the SGI handler:** `knl_define_inthdr(0, smp_resched_sgi_handler)` (writes `knl_intvec[0]`). **CAVEAT:** `knl_cpu_initialize` later zeroes `knl_intvec` (`cpu_init.c:28-30`) during T-Kernel boot — that is AFTER the self-test completes, so it does not affect the self-test, but it means the SGI handler is NOT registered for the production kernel (correct — the production kernel has no SMP). This is consistent with the sandbox boundary.
3. **Enable the boot CPU's OWN CPU interface:** `smp_gic_cpuif_init()` on the boot CPU.
4. **`vbar_el1`** — already set for the boot CPU in `start.S:101-104`; the secondary sets its own in `_secondary_disp_el1_setup` (`start.S:321-323`). Both CPUs share `el1_vectors` (`cpu_support.S:210-212`). So a delivered SGI reaches `_vec_el1_irq` on whichever CPU. ✓
5. **Unmask IRQs at the receiving CPU** for the cert window. The self-test today runs with IRQs masked (`_secondary_disp_el1_setup` never `daifclr`s; `smp_dispatch_run` never unmasks). **②.1a must `msr daifclr, #0x3` on the receiving CPU (B) at the point its LOW-prio task starts spinning** so the SGI can be taken — and re-mask on exit. This is a NEW, scoped IRQ-enable inside the sandbox; it must NOT leak into the production path (gated). An IRQ-enabled secondary could in principle take a stray timer PPI — but the secondary never programmed its own timer (it isn't in the self-test) and PMR/CTLR only route what's enabled, so SGI id 0 (the only `knl_intvec` slot registered) is the only thing it can act on. The cert confirms B took an SGI, not something else (the handler sets the flag only for INTID 0, since only `knl_intvec[0]` is registered).

**This §2.5 ordering is the part the auditor must independently re-derive and confirm against the running QEMU**, because the "IRQ doesn't work on aarch64 → suspect the vector/init before the device" lesson predicts the failure will be here (un-enabled CPU interface, masked DAIF, or distributor not enabled) rather than in the SGIR write itself.

---

## 3. Cross-CPU preemption semantics (the sandbox version)

### 3.1 The general semantics ②.1 models

In a real SMP scheduler: when CPU A readies a task T that is higher priority than what CPU B is currently running, A must (i) decide B is the CPU to preempt (in the general case, the lowest-priority currently-running CPU — a scan of `g_smpcpu[].ctxtsk` priorities under the lock), and (ii) `smp_send_reschedule(B)`. B takes the SGI, sets resched-pending, and at its next dispatcher check re-selects under the BKL, finding T now top of the ready list, and switches to it. This is the IPI-aware generalization of today's `knl_reschedule` (`task.h:240-249`), which on the uniprocessor just sets `knl_schedtsk = toptsk` and calls the no-op `knl_dispatch_request()` (`cpu_status.h:59`) — there is no second CPU to notify. **Converting the production `knl_reschedule` to be IPI-aware is ②.2** (it requires per-CPU `knl_schedtsk`, the 166-site conversion). ②.1 models the semantics in the sandbox only.

### 3.2 The sandbox shapes (concrete, for the cert)

②.1a extends the ②.0 self-test workload (`smp.c:417-450`, `smp_selftest_run`, `smp.c:506-572`) with two new task shapes and a directed scenario:

- **`smp_lowprio_spin_task`** (runs on B): a bounded loop (capped iterations so it can't wedge — the ②.0 watchdog discipline, e.g. `smp.c:363,560`) that, each iteration, checks `g_resched_pending[me]`. When the flag is set, it records "I yielded to a reschedule at iteration k" into a per-CPU record (extend `struct smp_cpu`, `smp.c:149-155`, with e.g. `volatile unsigned long preempted_at;` and a high-prio-ran flag) and RE-ENTERS the pull/select: under the BKL, `smp_ready_pull` now returns the high-prio task (which A pushed), B sets `g_smpcpu[me].ctxtsk = highprio` and runs it. This is the observable "B switched to the high-prio task."
- **`smp_highprio_task`** (the task A readies for B): a short task with a distinct id (e.g. `id = 999`) whose run records "the high-prio task executed on CPU B" (sets a flag the driver reads, and B's `g_smpcpu[me].ctxtsk` points at it). For the cert to PASS, this must execute on B AFTER B was preempted — i.e. `g_smpcpu[1].ctxtsk` ends pointing at the high-prio task AND `g_smpcpu[1].preempted_at != 0` AND the high-prio-ran flag is set.

**The scenario the driver orchestrates (extends `smp_selftest_run`, `smp.c:506`):**

1. Driver registers the SGI handler, enables distributor + boot CPU interface (§2.5).
2. Driver seeds the ready list with the LOW-prio spin task targeted at B, releases the secondary (`smp_bringup_secondary`, `smp.c:343-356` — unchanged), and the secondary enables its CPU interface (§2.3), unmasks IRQs (§2.5), pulls the low-prio task, and begins spinning + checking `g_resched_pending[1]`.
3. Driver (on boot CPU A) waits until B is provably spinning (B sets a "spinning live" flag, bounded-wait like `smp_wait_secondary_live`, `smp.c:361-373`).
4. Driver READIES the high-prio task: under the BKL, `smp_ready_push(&highprio)` (`smp.c:270-274`), then `smp_send_reschedule(1)` (§2.2).
5. Driver bounded-waits for the preemption to be observed: `g_resched_pending` was consumed, `g_smpcpu[1].preempted_at != 0`, high-prio-ran flag set, `g_smpcpu[1].ctxtsk == &highprio`. Watchdog → FAIL on timeout (`MAX_TRIES` idiom, `smp.c:363,560,568`).
6. Driver prints `SMP-PREEMPT: PASS/FAIL`.

**WHEN A decides to send:** in the sandbox, A sends unconditionally after readying the high-prio task (the scenario is directed). The general "scan `g_smpcpu[].ctxtsk` priorities, pick the lowest, send only if the readied task outranks it" decision is documented as the ②.2 production behavior but NOT implemented in ②.1a — the sandbox proves the mechanism, not the policy. **How B re-selects under the BKL:** exactly as ②.0 already does (`smp_dispatch_run` pulls under `bkl_acquire`/`bkl_release`, `smp.c:469-476`) — the only new thing is the *trigger* (the SGI-set flag) that makes B re-enter the pull while it was mid-task.

---

## 4. THE CERT `[smp-cross-preempt]` (cert-first, falsifiable)

### 4.1 Claim

On QEMU `-smp`, CPU B running a LOW-priority task is forced to switch to a HIGH-priority task — readied on CPU A and delivered by `smp_send_reschedule(B)` — **within a bounded time**, and this switch happens **only because the SGI was sent** (proven by the falsifier). This makes the GIC SGI IPI **load-bearing for cross-CPU preemption**.

### 4.2 Harness (extend `smp.c` driver + `run_smp0.sh` → a new `run_smp1.sh`)

The cert build: `make EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_PREEMPT_TEST"` (a new sub-flag so ②.0's `[smp-mutual-exclusion]` scenario and the ②.1 preempt scenario can be selected independently; or fold into the existing `SMP_SELFTEST` driver as an added phase printed after the ②.0 phases). The driver runs the §3.2 scenario and prints, alongside the ②.0 verdicts:

```
[SMP] cpu1 spinning on low-prio task (resched_pending=0)
[SMP] cpu0 readied high-prio task, sent reschedule SGI to cpu1
[SMP] cpu1 preempted_at=<k>  ran high-prio=<1>  ctxtsk=<&highprio>
SMP-PREEMPT: PASS          (B switched to the high-prio task after the SGI)
```

PASS condition (ALL must hold, watchdog-bounded so a miss is a FAIL not a hang):
- `g_smpcpu[1].preempted_at != 0` (B observed the resched while in the low-prio task),
- the high-prio-ran flag == 1 (the high-prio task actually executed on B),
- `smp_running_tcb(1) == &highprio` (B's per-CPU current task is the high-prio one),
- the whole thing completed before the watchdog (`MAX_TRIES`, `smp.c:363`).

`run_smp1.sh` (modeled on `run_smp0.sh`, `tests/aarch64/run_smp0.sh`): build under `-smp 4` (use CPU 0 + CPU 1), capture UART, grep `SMP-PREEMPT: PASS`, assert the kernel still reaches `Starting T-Kernel` (`main.c:361`) and `Initial task started` afterward (boot-survives, as `run_smp0.sh:83-86`).

### 4.3 The falsifier `-DSMP_NO_IPI` (MUST go RED)

Rebuild with `make EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_PREEMPT_TEST -DSMP_NO_IPI"`. Under `SMP_NO_IPI`, `smp_send_reschedule` compiles to a no-op (`(void)cpu;`, §2.2). EVERYTHING else is byte-identical: the high-prio task is still readied, B is still spinning with IRQs unmasked, the handler is still registered. But **no SGI is delivered** → `g_resched_pending[1]` never gets set → B's low-prio spin runs to its bounded cap WITHOUT ever re-selecting → `g_smpcpu[1].preempted_at == 0`, high-prio-ran == 0 → the bounded watchdog reports the miss → `SMP-PREEMPT: FAIL`. `run_smp1.sh` asserts `grep -q "SMP-PREEMPT: FAIL"` in the NO_IPI build (and that the kernel still boots — the missed preempt must not crash, only fail to preempt). **This proves the cert is not vacuous: the preemption happens because of, and ONLY because of, the IPI.** It is the exact analogue of ②.0's `-DSMP_MUTEX_NOLOCK` falsifier (`run_smp0.sh:90-108`, which proves the BKL is load-bearing by making the no-lock build lose updates).

### 4.4 QEMU-testable vs the honest RPi3 `[live]` caveat

| Sub-claim | QEMU `-smp 4` | RPi3 `[live]` |
|---|---|---|
| The SGI is sent, delivered, the handler runs on B, B re-selects to the high-prio task | **YES** (full — this is what `[smp-cross-preempt]` proves on QEMU) | yes |
| The `-DSMP_NO_IPI` falsifier goes RED (no send → no preempt) | **YES** (load-bearing proven) | yes |
| The **interrupt-delivery TIMING teeth** (real SGI latency, the `dsb ish` ordering of "readied-task-visible-before-interrupt" on weakly-ordered silicon) | **PARTIAL** — QEMU TCG models memory strongly and may MASK a missing `dsb ish` and may deliver the SGI with more forgiving timing than hardware | **ONLY here** for the real timing + barrier teeth |

**Honest statement for the cert row:** a QEMU `[smp-cross-preempt]` PASS proves *the SGI path is correctly plumbed and load-bearing* (send → receive → handler → re-select). It does **NOT** prove the barrier discipline (`smp_send_reschedule`'s `dsb ish`, §2.2) is correct on weakly-ordered silicon, nor the real cross-CPU interrupt latency — those are `[live]`-only on RPi3 hardware (the same MC-2 §4.4 / ②.0 honesty, `smp.c:64-68`, `run_smp0.sh:27-30`). **Additionally**, RPi3 does NOT use a GICv2 — it uses the BCM2837 ARM Local Interrupt Controller (`tkdev_init.c:29-70`), which has a **different IPI mechanism** (the per-core MAILBOX registers, not `GICD_SGIR`). So the `GICD_SGIR` send path is **QEMU-virt-GICv2-specific**; the RPi3 `[live]` port of `[smp-cross-preempt]` needs a BCM2837-mailbox `smp_send_reschedule` (a `BOARD_RPI3` `#ifdef`, mirroring how `gic_init`/`gic_enable_irq` already fork on `BOARD_RPI3`, `tkdev_init.c:29-111`). **State this explicitly: ②.1a is QEMU-virt-GICv2-only; the RPi3 mailbox IPI is a deferred `[live]` follow-up.**

---

## 5. The determinism + safety boundary (unchanged from ②.0)

1. **Shipped uniprocessor kernel byte-identical.** Every ②.1 symbol gated behind `SMP_SELFTEST` (the new `GICD_SGIR` define is harmless even when present — it's just a macro; but `smp_send_reschedule`, `smp_gic_cpuif_init`, `smp_resched_sgi_handler`, `g_resched_pending`, the new `struct smp_cpu` fields, and the driver scenario are ALL inside the existing `#ifdef SMP_SELFTEST … #endif` envelope of `smp.c:73…574`, `cpu_support.S:382…418`, `start.S:282…350`). The default `make` build produces an empty `smp.o` and an unchanged `_vec_el1_irq`. `.Ldispatch_loop` (`cpu_support.S:90-124`) and `task.c:62-63` globals are byte-untouched. **A `[smp-default-byte-identical]` guard (diff the default `kernel.elf` against a pre-②.1 reference, or at minimum confirm `nm`/size parity of the non-SMP TUs) should run in the cert** — the same discipline ②.0 relied on ("the DEFAULT build is BYTE-IDENTICAL to before", `Makefile:459`).
2. **The new SGI handler must not corrupt the production IRQ path when SMP is disabled.** When `SMP_SELFTEST` is off, `smp_resched_sgi_handler` does not exist and `knl_intvec[0]` stays NULL (`cpu_init.c:28-30`) — an SGI (which the production kernel never sends) would hit `cbz x3, .Lirq_no_dispatch` (`cpu_support.S:268`) and be harmlessly EOIRed, exactly as today. The `_vec_el1_irq` vector itself is NOT edited by ②.1 (the SGI is handled purely via the EXISTING `knl_intvec` dispatch, `cpu_support.S:264-269`) — this is the key safety property: **②.1 adds a handler to an existing table slot; it does not rewrite the interrupt vector.** This is the lowest-risk way to thread the GIC's recurring IRQ-path C-ABI trap.
3. **The secondary's GIC init must not perturb the boot CPU's existing GIC setup.** `smp_gic_cpuif_init` (§2.3) writes ONLY the per-CPU banked `GICC_PMR`/`GICC_CTLR` — it does **not** touch the shared `GICD_CTLR`/`GICD_ISENABLER` (the distributor the boot CPU owns). The CPU interface is per-core hardware: enabling CPU 1's interface cannot disturb CPU 0's. The one shared write (the distributor enable, §2.5 step 1) is idempotent with `gic_init`'s `GICD_CTLR=1` (`tkdev_init.c:99`) — it sets the same bit, and the later `gic_init` re-sets it harmlessly during T-Kernel boot. **State explicitly: ②.1 makes exactly ONE shared-GIC write (distributor enable, idempotent) and otherwise only per-CPU-banked writes.**
4. **The BKL still serializes kernel state.** ②.1's re-select on B happens under `bkl_acquire`/`bkl_release` (the ②.0 lock, `smp.c:215-239`) — the SGI handler only sets a flag (no shared-state mutation; `smp_resched_sgi_handler` touches only `g_resched_pending[me]`, per-CPU), and the actual pull/select is the ②.0 BKL-guarded path. No new shared mutable state is raced unlocked. The `g_resched_pending[]` array is per-CPU (one writer = the handler on that CPU, one reader = the dispatcher on that CPU) so it needs only a `dmb`, not the BKL.
5. **The mind path is untouched.** ②.1 adds GIC IPI plumbing in the SMP sandbox; it reads/writes no mind weights, no engram ring, no `gl_merge`. The `[smp-one-mind]` crown cert is **②.2** (it needs the production scheduler under SMP). ②.1's only crown obligation is the negative Boundary 1 (don't regress default byte-identity), guarded by §5.1.

---

## 6. Honesty (lead with the limits)

1. **②.1 adds REAL, genuinely-new interrupt-controller work.** The GIC SGI path does not exist on trunk — proven: the only `SGIR`/`SGI`/`IPI` token under `arch/aarch64/` is the deferral comment `smp.c:40`; `tkdev_conf.h:32-38` has no `GICD_SGIR`; `gic_init` (`tkdev_init.c:93-109`) plumbs only the timer PPI. This is the first hardware-facing piece of ②: ②.0 was shared-memory cooperation, ②.1 delivers a cross-CPU hardware interrupt. The implementer is writing a `GICD_SGIR` driver, a per-CPU CPU-interface init, and an SGI handler from nothing.
2. **The barrier/IRQ-timing teeth are only fully `[live]` on RPi3 — and RPi3 isn't even GICv2.** QEMU TCG models memory strongly and may MASK a missing `dsb ish` in `smp_send_reschedule` (§2.2) and delivers SGIs with more forgiving timing than silicon (the MC-2 §4.4 / ②.0 honesty, `smp.c:64-68`). Worse, **RPi3 uses the BCM2837 local controller's mailbox IPIs, not `GICD_SGIR`** (`tkdev_init.c:29-70` shows the RPi3 GIC fork is an entirely different controller) — so the `[live]` port of `[smp-cross-preempt]` needs a separate BCM2837-mailbox send path (`BOARD_RPI3` `#ifdef`, mirroring the existing `gic_init` fork). **②.1a is QEMU-virt-GICv2-only; a QEMU green is NOT a hardware green.**
3. **②.1 is still the self-test SANDBOX; the production scheduler conversion is ②.2.** ②.1 sends its reschedule SGI to the ②.0 self-test dispatcher (`smp_dispatch_run`) over the ②.0 self-test tasks (`struct smp_task`). It does NOT convert the ~166 production `knl_ctxtsk`/`knl_schedtsk` sites to per-CPU, does NOT make the production `knl_reschedule` (`task.h:240-249`) IPI-aware, and does NOT perform a true asynchronous register-context preempt inside the SGI handler (②.1a's preemption is "cooperative-at-a-checkpoint" — B's low-prio task checks the resched flag at loop boundaries). The full async context switch + the 166-site conversion + the `[smp-one-mind]` crown are **②.2** (§7). **What ②.1 proves is exactly: the IPI mechanism is real and load-bearing.** That is the honest, bounded claim.
4. **This plan makes ②.1 implementation-ready; it does not start it.** Read-only on code. The implementation is a separate impl→audit→integrate cycle on this same base.

---

## 7. Sequencing + honest deferral

Each step: a falsifiable cert + a falsifier that MUST go RED, on the **explicit-hash base `9db8a998`**, via a **separate impl→audit cycle** (implementer ≠ auditor ≠ commander).

**②.1a — the GIC SGI send/receive plumbing + `[smp-cross-preempt]` on QEMU `-smp`. (THE SMALLEST REAL ②.1 SLICE.)**
- Add `GICD_SGIR 0xF00` (`tkdev_conf.h`); `smp_send_reschedule(cpu)` (writes SGIR; no-op under `-DSMP_NO_IPI`) (§2.2).
- Add `smp_gic_cpuif_init()` — per-CPU `GICC_PMR`/`GICC_CTLR` enable on each CPU; one idempotent distributor enable from the boot CPU (§2.3, §2.5).
- Add `smp_resched_sgi_handler` (sets per-CPU `g_resched_pending[me]`) registered in `knl_intvec[0]` via `knl_define_inthdr` (§2.4).
- Extend the self-test workload with `smp_lowprio_spin_task` (checks the flag, re-selects) + `smp_highprio_task` + the directed scenario in `smp_selftest_run` (§3.2). Enable IRQs on B for the cert window (§2.5 step 5).
- **Cert `[smp-cross-preempt]`:** B preempts to the high-prio task after the SGI → `SMP-PREEMPT: PASS`. **Falsifier `-DSMP_NO_IPI`:** no send → B never preempts → `SMP-PREEMPT: FAIL` (§4). New `tests/aarch64/run_smp1.sh` + `run-smp1`/`smp1-test` Makefile targets, modeled on `run_smp0.sh` / `smp0-test` (`Makefile:460-477`). Keep N=2 (reuse ②.0's single secondary).
- **Guard:** `[smp-default-byte-identical]` — the default build stays byte-identical (§5.1).

**②.1b — idle-CPU wakeup-via-SGI + the harness-fragility fix.**
- **Idle-CPU wakeup:** today a CPU that drains its work idles in `wfe` forever (`smp_dispatch_run` tail, `smp.c:491-493`; `smp_dispatch_loop` `wfe` loop, `cpu_support.S:416-417`). ②.1b lets CPU A GIVE work to a sleeping CPU B by `smp_send_reschedule(B)` — the SGI wakes B from `wfe` (an SGI is a `wfe` wake event once B's CPU interface is enabled) and B re-pulls. This is the "a sleeping CPU can be given work" capability, distinct from ②.1a's "preempt a busy CPU." Cert: seed work AFTER B is idling in `wfe`; prove B wakes and runs it (watchdog-bounded). Same `-DSMP_NO_IPI` falsifier (no send → B sleeps forever → watchdog FAIL). Extend to N=4 here (`mc2_smp_release_n`, `main.c:41`) — multiple idle secondaries each wakeable.
- **Harness-fragility fix (from ②.0's audit):** `run_smp0.sh` boots `-kernel kernel.elf` directly from `$BOOT` (`run_smp0.sh:40,45-60`; `Makefile:463` `QEMU_SMP_FLAGS … -kernel kernel.elf`). A concurrent `make` (e.g. the falsifier rebuild, `run_smp0.sh:92-94`) overwrites `kernel.elf` while a QEMU may still be reading it — a race the worktree-agent fleet hits. **Fix:** snapshot the built ELF to a unique temp path (`cp kernel.elf "$(mktemp)"`) BEFORE booting, and point QEMU at the snapshot, so a later rebuild can't corrupt an in-flight run. Apply to both `run_smp0.sh` and the new `run_smp1.sh`. (Pure test-harness robustness; no kernel code change.)

**DEFERRED to later (explicit):**
- **②.2 — convert the production `knl_ctxtsk`/`knl_schedtsk` to per-CPU** (the ~166 reader sites; per-CPU accessor macros over the production dispatcher `.Ldispatch_loop`) + make the production `knl_reschedule` (`task.h:240-249`) IPI-aware + the TRUE asynchronous register-context preempt inside the SGI handler (save B's full context, switch stacks) + **the `[smp-one-mind]` byte-identity CROWN cert** (the real student `r_forward`/DMN/`gl_merge` byte-identical under SMP vs uniprocessor). This is the gate that says "② did not split the mind." ②.1 does NOT touch it.
- **②.3 — finer locks** (split the BKL → `g_rqlock` → `g_memlock` → object-table locks) + per-CPU run-queues + migration/load-balancing. Re-run `[smp-one-mind]` after every split.
- **Hosted-port SMP** (threads-as-CPUs): the IPI analogue is a per-thread real-time signal (`pthread_kill(tid, SIGRESCHED)`), mirroring the existing `SIGEV_THREAD_ID` per-thread targeting. Separate lift.
- **RPi3 `[live]`** — the BCM2837 mailbox IPI send path (`BOARD_RPI3` `#ifdef` in `smp_send_reschedule`) + real barrier/timing teeth on hardware. The `GICD_SGIR` path is QEMU-virt-GICv2-only.

**②.1 STILL RUNS THE SELF-TEST SANDBOX.** It proves the IPI mechanism over the ②.0 self-test tasks. The production scheduler conversion (②.2) is deferred — stated here, in the cert, and in §6.

---

## Appendix — grounding (file:line, all on the shared checkout at trunk `9db8a998`)

**The ②.0 base ②.1 builds ON (sandbox, all `#ifdef SMP_SELFTEST`):**
- The BKL: `arch/aarch64/smp.c:189-239` (`g_bkl_lock`/`g_bkl_owner`/`g_bkl_depth`, `raw_lock`/`raw_unlock`, recursive `bkl_acquire`/`bkl_release`).
- Per-CPU block `g_smpcpu[]` (ctxtsk/schedtsk/exec_count/cpu_id/live): `smp.c:149-168`; asm-mirrored offsets `SMPCPU_*` `smp.c:157-164` and `cpu_support.S:387-389` (`_Static_assert` guard `smp.c:162-164`); `SMP_MAX_CPUS 2` `smp.c:147`.
- Per-CPU dispatcher: `smp_dispatch_run` `smp.c:453-494`; `smp_dispatch_loop` (asm landing pad → C) `cpu_support.S:413-417`; per-CPU current-task load `smp_cur_tcb_load` `cpu_support.S:400-407`.
- Shared ready list: `g_ready[]`/`smp_ready_push`/`smp_ready_pull` `smp.c:264-295`.
- Bringup: `smp_bringup_secondary` (PSCI CPU_ON → `_secondary_dispatch_entry`) `smp.c:343-356`; the new landing pad `_secondary_dispatch_entry` + `_secondary_disp_el1_setup` `start.S:282-350`; own copies of `smp_psci_cpu_on`/`smp_set_smpen`/`smp_mpidr_aff0` `smp.c:90-125`.
- Self-test driver + workload: `smp_selftest_run` `smp.c:506-572`; `smp_run_task`/`smp_counter_inc`/barrier `smp.c:313-450`; watchdog idiom (`MAX_TRIES`/bounded join) `smp.c:361-373,558-571`; observability helpers `smp_wait_secondary_live`/`smp_exec_count`/`smp_running_tcb`/`smp_get_counter` `smp.c:361-393`.
- The ②.0 cert harness + falsifier: `tests/aarch64/run_smp0.sh` (build `-DSMP_SELFTEST` `:65-66`, grep `SMP-RUN/MUTEX/BOOT PASS` `:75-87`, NOLOCK falsifier `:90-108`); Makefile targets `run-smp0`/`run-smp0-nolock`/`smp0-test` `Makefile:460-477`; the byte-identity claim `Makefile:459`.
- The boot self-test driver: `boot/aarch64/main.c:303-359` (runs BEFORE `knl_t_kernel_main` at `:361`); externs `:75-80`.
- Gating: `smp.c:73` `#ifdef SMP_SELFTEST`; `cpu_support.S:382`; `start.S:282`; smp.c compiled always but empty without the flag (`Makefile:106`).

**The GIC SGI/IPI gap (the real ②.1 work):**
- No SGI plumbing — `gic_init` does distributor + timer PPI + CPU-iface + PMR only: `tkdev_init.c:93-109` (`GICD_CTLR=1` `:99`, timer PPI `:102`, `GICC_PMR=0xFF` `:105`, `GICC_CTLR=1` `:106`); `gic_enable_irq` (writes `GICD_ISENABLER`) `:80-86`.
- No `GICD_SGIR` (0xF00): `tkdev_conf.h:32-38` (defines `GICD_CTLR`/`GICD_ISENABLER`/`GICC_CTLR`/`GICC_PMR`/`GICC_IAR`/`GICC_EOIR` — no SGIR); bases `GICD_BASE 0x08000000`/`GICC_BASE 0x08010000` `:28-29`; grep `arch/aarch64/` for `sgir|sgi|ipi` → only `smp.c:40` (the deferral comment).
- The IRQ vector (receives SGI via IAR, nothing sends, no handler registered): `_vec_el1_irq` `cpu_support.S:234-284` (IAR read `:256`, mask 10 bits `:258`, spurious-≥512 drop `:259-261`, `knl_intvec[INTID]` dispatch `:264-269`, EOIR `:280`); shared `el1_vectors` `:210-232`.
- The interrupt-handler table: `knl_intvec[N_INTVEC]` def `cpu_init.c:14` (zeroed `:28-30`); `knl_define_inthdr` `arch/aarch64/include/cpu_insn.h:53-58` (bounds-checked `vecno < N_INTVEC`); the timer registration precedent `tkdev_init.c:160`; `N_INTVEC 512` `tkdev_conf.h:49`.
- RPi3 uses a DIFFERENT controller (BCM2837 local, mailbox IPIs — not GICv2): `tkdev_init.c:29-70` (`gic_init`/`gic_enable_irq` `BOARD_RPI3` fork); GIC bases differ `tkdev_conf.h:24-26`.

**The production reschedule mechanism (what ②.2 — NOT ②.1 — will drive):**
- Scheduler globals (the uniprocessor "one running / one next"): `kernel/common/task.c:62-63` (`knl_ctxtsk`/`knl_schedtsk`), `:55` (`knl_dispatch_disabled`), `:64` (`knl_ready_queue`).
- `knl_reschedule` sets the single `knl_schedtsk`, calls no-op `knl_dispatch_request`: `include/kernel/tkernel/task.h:240-249`; `knl_dispatch_request()` = no-op `arch/aarch64/include/cpu_status.h:59`.
- The production dispatcher (loads GLOBAL `knl_schedtsk`/`knl_ctxtsk`; per-CPU idle `.Lidle`): `cpu_support.S:90-124`.
- Critical-section primitives (DI/EI = the uniprocessor lock; dispatch on `END_CRITICAL_SECTION`): `arch/aarch64/include/cpu_status.h:17-24`; the timer-handler critical section over `knl_timer_queue` + RR slice: `kernel/common/timer.c:177-231` (`BEGIN_CRITICAL_SECTION` `:183`, RR `:219-226`, `END` `:228`).
- The timer tick path (drives preemption today): `tkdev_init.c:117-148` (`timer_init` PPI program, `timer_irq_handler` reload + `knl_timer_handler_startup`); `knl_timer_handler_startup` (asm, `taskindp++/--`) `cpu_support.S:151-170`.

**The boundary (byte-identity / gating):**
- Default build byte-identical (no `-DSMP_SELFTEST` → empty smp.o, unchanged dispatcher + IRQ vector): `Makefile:459`; gating envelopes `smp.c:73…574`, `cpu_support.S:382…418`, `start.S:282…350`.
- Linker secondary stacks (reused): `boot/aarch64/linker.ld:44-69` (`_stack_top` `:47`, `_stack_top_cpu1` `:56`).

---

**Note:** This plan is a DESIGN PLAN by an automated design-harden on trunk `9db8a998` (②.0 merged + audited MERGEABLE). ②.1's implementation awaits **commander review** + a **separate impl→audit cycle** (implementer ≠ auditor ≠ commander; the development METHOD is the project's immune system). This plan makes ②.1 READY and de-risked — it does **not** start it. The smallest real first slice is **②.1a** (GICD_SGIR + `smp_send_reschedule` + per-CPU GIC CPU-interface init + the reschedule-SGI handler; `[smp-cross-preempt]` PASS with the `-DSMP_NO_IPI` falsifier going RED; N=2; still the ②.0 self-test sandbox; production scheduler conversion = ②.2, deferred).
