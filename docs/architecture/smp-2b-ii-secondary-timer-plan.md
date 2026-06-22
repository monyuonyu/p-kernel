# ②.2b-ii — secondary-CPU CNTP timer + WAIT-state support: implementation plan (cert-first, adversarially hardened)

> **DESIGN ONLY.** This document is the plan for the next ② slice. It modifies
> no kernel code. It is hardened across three lenses (byte-identity / deadlock /
> wait-state) before any implementation agent touches the tree.
>
> **Base commit:** `2f395561` (the ②.2c design plan; ②.2b-i + the §5.4 BKL guard
> already shipped at `a76f3606`). **Default `.text` sha that MUST NOT MOVE:**
> `755a20fae2d9b741…` (`docs/audit-trail.md:997`).

---

## 0. THE HEADLINE FINDING (read this first)

**②.2b-ii is the FIRST ② slice whose correct implementation requires changing the
behavior of a function that is LINKED INTO THE DEFAULT, SHIPPED BUILD — namely
the cross-CPU wakeup, which lives in `knl_make_ready` (`kernel/common/task.c:226`)
and reaches it through ~18 `knl_wait_release_ok` call sites across every
synchronization object.** Every prior ② slice (②.0 … ②.2b-i) added behavior in
`SMP_SELFTEST`-gated TUs and in `#ifdef SMP_SELFTEST` arms of the asm, so the
default `.text` stayed byte-identical *for free*. ②.2b-ii cannot get the
cross-CPU wake for free the same way, because the place that needs to become
"IPI the OTHER CPU" is a shared-core C function.

**The good news, stated up front so it is not buried:** the byte-identity CAN be
preserved, but ONLY because the new cross-CPU behavior can be expressed as a call
that is *itself* a macro/inline which is **empty when `SMP_SELFTEST` is off**, in
exactly the same pattern `BKL_ACQUIRE()`/`CUR_SCHEDTSK` already established
(`arch/aarch64/include/cpu_status.h:23-41`, `include/kernel/tkernel/task.h:193-209`).
The hard requirement — and the single biggest risk this plan surfaces — is:

> **§LENS-A RISK (biggest):** the cross-CPU wake hook added to `knl_make_ready`
> MUST be a textually-empty no-op under the non-SMP build (a `knl_smp_wake_hook()`
> macro that `#define`s to nothing without `SMP_SELFTEST`), AND the
> implementation MUST re-prove the default `.text` sha `755a20fae2d9b741…` AFTER
> the edit. If the hook cannot be made to vanish — e.g. if it must read a
> per-CPU array unconditionally, or if the compiler emits a different
> instruction schedule around an "empty" inline that takes the TCB pointer — then
> **byte-identity is broken and the slice must stop and escalate**, not ship a
> "close enough" `.text`. The §6 byte-identity strategy names the exact gating
> for every shared-core touch; §LENS-A enumerates which functions are at risk
> and which are already covered by the existing `CUR_*` macro.

The secondary CNTP timer half (`§3`) is byte-identity-trivial — it lives entirely
in `SMP_SELFTEST`-gated SMP TUs and per-CPU-banked hardware registers, like every
prior slice. The WAIT/cross-CPU-wake half (`§4`) is where the danger is.

---

## 1. The cert, stated first (cert-first)

### 1.1 `[smp-secondary-wait]` — the two halves

**`[smp-secondary-wait]`** — a real T-Kernel task running on the **secondary**
(CPU 1) can BLOCK and WAKE:

- **(i) self-timer wake (proves the secondary's OWN CNTP fires):** a task on
  CPU 1 calls `tk_dly_tsk(N)` and is woken **by CPU 1's own timer tick**. This
  proves the secondary programs and takes its own EL1 generic-timer interrupt
  (CNTP PPI 30 is per-CPU by construction).
- **(ii) cross-CPU wake (proves cross-CPU wakeup → mark-ready → IPI → resume):**
  a task on CPU 1 blocks on a **semaphore** (`tk_wai_sem`, infinite timeout) that
  **CPU 0 signals** (`tk_sig_sem`). CPU 0's signal readies the task; an IPI to
  CPU 1 makes CPU 1 re-dispatch it; the task resumes on CPU 1.

Print **`SMP-SECONDARY-WAIT: PASS`** when both hold.

> This is a deliberate sharpening of the `[smp-secondary-sleep]` cert sketched in
> `docs/architecture/smp-2b-async-preempt-plan.md:17,166-197` (which proved only
> half (i)). The added half (ii) is the *cross-CPU* path — the genuinely new and
> dangerous mechanism — and is what makes the cert non-vacuous against the
> "CPU 0's tick happened to do the work" failure (§1.3).

### 1.2 The falsifiers (each MUST go RED)

1. **`-DSMP_NO_SEC_TIMER`** — build with the secondary's CNTP **NOT** programmed
   (skip the per-CPU `timer_init`-equivalent and the per-CPU PPI 30 enable on
   CPU 1). Then half (i)'s `tk_dly_tsk` task on CPU 1 never receives a tick that
   it raised itself; **the test is constructed (§1.3) so no other CPU's tick can
   wake it** → the sleeping task hangs forever → watchdog → **`SMP-SECONDARY-WAIT:
   FAIL`**. Proves the secondary timer is load-bearing.
2. **`-DSMP_NO_XWAKE`** — suppress the cross-CPU IPI in the wake path (the
   `smp_send_reschedule(target)` after `knl_make_ready` becomes a no-op). Then in
   half (ii) CPU 0 signals the semaphore and marks the waiter ready, but CPU 1 is
   never told to re-dispatch → the sem-waiter never wakes → watchdog →
   **`SMP-SECONDARY-WAIT: FAIL`**. Proves the cross-CPU IPI is load-bearing.

### 1.3 NON-VACUITY — the central design obligation

The cert **must not be able to pass if CPU 1's own tick never fired.** The naive
failure mode: half (i) queues the delayed task's `wtmeb` into the **single, shared
`knl_timer_queue`** (`kernel/common/timer.c:51`); the boot CPU's tick ALSO walks
that same queue (`knl_timer_handler`, `timer.c:177-231`); so CPU 0's tick could
fire `knl_wait_release_tmout` for the secondary's delay even with the secondary's
own timer dead. That would let `-DSMP_NO_SEC_TIMER` PASS — a vacuous cert.

**The construction that forces only CPU 1's tick to wake the half-(i) task:**

- **Disable the boot CPU's tick-driven timeout service for the half-(i) window,
  OR keep CPU 0 out of the timer-queue walk while the secondary's delay is
  pending.** Concretely (the conservative choice): in the
  `SMP_SECONDARY_WAIT`-gated driver, after the secondary queues its delay, the
  boot CPU does **not** run a competing tick over the shared queue during the
  measurement window — it spins in the watchdog with its own timer interrupt
  *masked* (`smp_irq_mask()` analog), so the ONLY core whose `knl_timer_handler`
  can dequeue the secondary's `wtmeb` is CPU 1. If the secondary's CNTP is dead
  (`-DSMP_NO_SEC_TIMER`), nobody walks the queue → the delay never expires → FAIL.
  This is the cleanest non-vacuity guarantee: physically remove the alternative
  waker.
- **Plus a positive witness:** the secondary's `timer_irq_handler` bumps a
  per-CPU `g_sec_tick_count[1]` (observability, like `g_sgi_taken[]`,
  `smp.c:295`). The cert PASS additionally requires `g_sec_tick_count[1] >= 1`,
  so a PASS literally cannot be claimed unless CPU 1 took at least one of its own
  ticks. (Half (ii) uses CPU 0's signal, not a tick, so it is independent of this
  and cannot mask a dead secondary timer.)

> **Auditor note (cert-must-cover-all-paths, MEMORY.md
> `feedback_cert_must_cover_all_paths`):** confirm against the running QEMU that
> in the `-DSMP_NO_SEC_TIMER` build the half-(i) task TRULY hangs (watchdog
> elapses), not "wakes a little late via CPU 0." The non-vacuity is the whole
> point of half (i); a falsifier that still passes is the worst outcome.

### 1.4 Cert build gating

The cert TU is `arch/aarch64/smp_secwait.c` (new), empty unless
`-DSMP_SECONDARY_WAIT` (which `#error`s without `SMP_SELFTEST`, mirroring
`smp_async.c:77-80`). It is added to `SMP_CERT_EXCLUDE` in `boot/aarch64/Makefile`
(`:245-258`) so it is **dropped from `LINK_OBJS` in the default build** — the
shipped ELF never links it. New Makefile targets `smp-secwait` /
`smp-secwait-no-sectimer` / `smp-secwait-no-xwake` mirror the existing
`smp-async*` targets (`Makefile:588-600`), with a new `tests/aarch64/run_smp4.sh`
harness modeled on `run_smp3.sh`.

---

## 2. Where ②.2b-i left us, and the exact two things ②.2b-ii adds

**②.2b-i shipped** (`a76f3606`): the production IRQ-return path in `_vec_el1_irq`
(`arch/aarch64/cpu_support.S:350-393`) performs a **true async register-context
switch** when `smp_irq_need_resched()` (`smp.c:594-655`) returns 1 — its 5
clauses are pending-flag, BKL-not-held (the §5.4 guard, `smp.c:632`),
not-task-independent, dispatch-not-disabled-on-me, and `ctxtsk != schedtsk`. The
resume path `.Lirq_resume_tramp` (`cpu_support.S:411-418`) round-trips
`ELR_EL1`/`SPSR_EL1` + the 160-byte caller frame. **This whole machinery is the
re-dispatch primitive ②.2b-ii reuses** — once a secondary's `CUR_SCHEDTSK` says
"a different task should run," the existing async hook switches to it on the next
IRQ. ②.2b-ii does not invent a new switch; it makes the *timer tick* a source of
that IRQ on the secondary, and makes a *cross-CPU signal* set the secondary's
`CUR_SCHEDTSK` + ring its IRQ doorbell.

**②.2b-i's honest deferral, verbatim** (`smp_async.c:66-74`):
> *"②.2b-ii (the secondary's own CNTP PPI 30 tick + the cross-CPU wake gap so a
> secondary task can tk_dly_tsk/tk_slp_tsk and WAKE …) is HONESTLY DEFERRED: it
> is a second, independent mechanism (per-CPU banked timer enable + making
> knl_make_ready IPI-aware) with its own fault surface."*

And the precise reason a secondary blocking syscall faults TODAY
(`smp_prod.c:88-99`):
> *"the secondary's TIMER/WAIT path (its own EL1 timer PPI + tick-driven
> reschedule) is NOT wired in ②.2a … B must NOT call a blocking/timer syscall
> here (tk_dly_tsk/tk_slp_tsk would enter the unwired secondary wait path and
> fault)."*

So the boundary is exact. **②.2b-ii = (A) program + take the secondary's own CNTP
PPI 30 tick, and (B) close the cross-CPU wake-dispatch gap.**

---

## 3. Half A — the per-secondary CNTP timer (the byte-identity-trivial half)

### 3.1 What the boot CPU does today, and why the secondary gets nothing

The boot CPU's timer is set up ONCE inside `knl_t_kernel_main` on CPU 0:
- `timer_init()` (`arch/aarch64/tkdev_init.c:117-133`) reads `CNTFRQ_EL0`, writes
  `CNTP_TVAL_EL0 = freq/TIMER_HZ`, and `CNTP_CTL_EL0 = 1` (enable, unmasked).
- `gic_init()` (`tkdev_init.c:93-109`) enables the shared distributor
  (`GICD_CTLR=1`), enables PPI 30 (`gic_enable_irq(INTNO_TIMER_GIC)`,
  `:101-102`), and enables **the boot CPU's** CPU interface (`GICC_PMR=0xFF`,
  `GICC_CTLR=1`, `:105-106`).
- `timer_irq_handler` (`tkdev_init.c:138-148`) is registered globally via
  `knl_define_inthdr(INTNO_TIMER_GIC, …)` (`:160`) into `knl_intvec[30]`, so it is
  shared by every CPU that reaches `_vec_el1_irq` → `knl_intvec[INTID]`
  (`cpu_support.S:331-336`).

`CNTP_*_EL0` and `CNTP_CTL_EL0` are **per-CPU-banked system registers**; PPI 30 is
a **per-CPU interrupt**; the PPI-enable bits in `GICD_ISENABLER0` are
**per-CPU-banked** on GICv2. So the secondary has its OWN banked copies, all
unprogrammed/disabled at bringup → the secondary never takes a tick. The
`knl_intvec[30]` handler is already global, so once the secondary's PPI 30 is
enabled and its CNTP is armed, **the existing handler `timer_irq_handler` runs on
CPU 1 with zero new dispatch code** — it reloads the local `CNTP_TVAL_EL0` (a
per-CPU write, `tkdev_init.c:144`) and calls `knl_timer_handler_startup` →
`knl_timer_handler`.

### 3.2 What Half A adds (Option T1 — per-CPU CNTP; PINNED)

This plan **pins Option T1** (each CPU programs its own CNTP) over T2 (boot CPU
ticks for all), for the reasons in `smp-2b-async-preempt-plan.md:177-185`: T1 is
faithful to "each CPU runs an independently tick-preemptible task," the hardware
is already banked, and it gives the secondary RR time-slicing for free. T2 is
recorded as the RPi3 `[live]` fallback (BCM2837 is not GICv2; §7).

New code, **all in `SMP_SELFTEST`-gated TUs** (no shared-core edit):

1. A `smp_secondary_timer_init(void)` in `smp.c` (or a small new `smp_sectimer.c`)
   that, on the secondary, does the per-CPU subset of `timer_init`/`gic_init`:
   - enable PPI 30 in the secondary's banked `GICD_ISENABLER0`
     (`gic_enable_irq(INTNO_TIMER_GIC)` equivalent — but header-light, mirroring
     `smp_gic_cpuif_init`, `smp.c:321-328`);
   - it already enables its CPU interface via `smp_gic_cpuif_init` (called for the
     SGI, `smp.c:321`) — reuse it;
   - program `CNTP_TVAL_EL0 = freq/TIMER_HZ` + `CNTP_CTL_EL0 = 1` (the 4
     instructions of `tkdev_init.c:126-129`, replicated header-light).
   Called by the secondary from its `smp_dispatch_run` arm (`smp.c:1096+`) in the
   new `#ifdef SMP_SECONDARY_WAIT` block, BEFORE it enters the production
   dispatcher and unmasks IRQ.
2. The secondary unmasks IRQ/FIQ (`smp_irq_unmask`, `smp.c:381`) so PPI 30 can be
   taken — exactly as the async cert's L task does (`smp_async.c:161`).
3. `g_sec_tick_count[SMP_MAX_CPUS]` observability bumped from the secondary's
   tick (the positive non-vacuity witness, §1.3). **Where to bump it without a
   shared-core edit:** wrap — do NOT modify `timer_irq_handler`. Register a
   *secondary-specific* `knl_intvec[30]` shim ONLY for the cert window? No — that
   would perturb CPU 0's shared handler. **Correct approach:** bump the counter
   from inside `knl_timer_handler`'s existing `CUR_CTXTSK` charge path? No — that
   is shared core. **The clean answer:** the per-CPU tick count is incremented by
   a tiny `SMP_SELFTEST`-gated hook the secondary registers as a SECOND handler
   it owns, or simpler — the secondary reads `knl_current_time` deltas as its
   witness instead of a counter (the cert already needs `knl_current_time` to
   advance, §4.4). **PIN: use `knl_current_time` advance as the positive witness
   to avoid ANY shared-core counter edit;** see §LENS-A item 5 and §6.

> **Byte-identity for Half A: TRIVIAL.** Every symbol above is in a
> `SMP_SELFTEST` TU; the secondary writes only per-CPU-banked hardware
> (`CNTP_*_EL0`, the banked `GICD_ISENABLER0` PPI bit, `GICC_*`), so it cannot
> perturb CPU 0's timer/GIC even at runtime, and at N=1 (no secondary) none of it
> executes. `tkdev_init.c` / `timer.c` are **not edited**. The §5.3 GICv2
> PPI-banking obligation is inherited from ②.1a (`smp.c:317-328`) and must be
> re-confirmed against the running QEMU.

### 3.3 The CNTFRQ / cadence caveat

QEMU virt CNTFRQ is 62.5 MHz, RPi3 is 19.2 MHz; `freq/TIMER_HZ` is computed
per-CPU from the local `CNTFRQ_EL0` (`tkdev_init.c:120-123`), which is correct on
both. The secondary's tick cadence equals the boot CPU's. No new cadence math.

---

## 4. Half B — the WAIT / cross-CPU-wake path (the dangerous half)

### 4.1 Why `tk_dly_tsk` / `tk_wai_sem` are NOT inherently broken on a secondary

Trace `tk_dly_tsk_impl` (`time_calls.c:136-153`): it `BEGIN_CRITICAL_SECTION`s
(masks IRQ + acquires the BKL under SMP, `cpu_status.h:52`), sets
`CUR_CTXTSK->wspec/wid/wercd`, calls `knl_make_wait_reltim` (`wait.c:220-231`)
which calls `knl_make_non_ready(CUR_CTXTSK)` (`task.c:253-260`), sets
`CUR_CTXTSK->state = TS_WAIT`, and `knl_timer_insert_reltim`s the task's `wtmeb`
into the **shared** `knl_timer_queue` (`timer.c:140-150`). Then
`END_CRITICAL_SECTION` (`cpu_status.h:53-60`): `knl_make_non_ready` set
`CUR_SCHEDTSK = knl_ready_queue_top(...)` (`task.c:256-257`), so now
`CUR_CTXTSK != CUR_SCHEDTSK` → it calls `knl_dispatch()` → **the secondary
correctly switches OFF the blocking task using the existing per-CPU
`.Ldispatch_loop`.** Every one of those reads/writes resolves to the secondary's
own per-CPU slot via the `CUR_*` macros, and every shared structure
(`knl_timer_queue`, `knl_ready_queue`, the TCB) is BKL-serialized. **So the BLOCK
side already works on a secondary today** — the ②.2a `CUR_*` conversion covers it.
The fault `smp_prod.c:88` warns about is purely the **WAKE** side.

### 4.2 The two wake gaps

- **Gap #1 (Half A, already solved):** the delay only expires when SOME CPU's
  `knl_timer_handler` walks `knl_timer_queue`. Half A gives the secondary its own
  tick, so the secondary walks the queue and fires `knl_wait_release_tmout`
  (`wait.c:112-178`) on its OWN task — entirely on CPU 1, no cross-CPU step. For
  half (i) this is the WHOLE story: CPU 1 blocks, CPU 1's tick wakes it. Good.

- **Gap #2 (the real new hazard — cross-CPU wake):** when **CPU 0** releases a
  task that belongs on **CPU 1** — e.g. `tk_sig_sem` on CPU 0 →
  `knl_wait_release_ok(tcb)` (`semaphore.c:225`) → `knl_wait_release`
  (`wait.h:156-162`) → `knl_make_non_wait` → **`knl_make_ready(tcb)`**
  (`task.c:226-233`) — `knl_make_ready` does `CUR_SCHEDTSK = tcb`
  (`task.c:230`). **But `CUR_SCHEDTSK` resolves to the CALLING CPU's slot
  (`g_smpcpu[0].schedtsk`), not the slot of the CPU the woken task should run
  on.** Nothing sets `g_smpcpu[1].schedtsk` and nothing tells CPU 1 to
  re-dispatch. The woken task sits READY in the shared `knl_ready_queue`, but CPU
  1 is parked and never re-selects. **This is the gap.** It is reached by ~18 call
  sites: `knl_wait_release_ok` in semaphore/eventflag/mailbox/mempfix/mempool/
  messagebuf/mutex/rendezvous (`grep` enumerated 16) plus `knl_make_ready`
  directly in `task_sync.c:125,172` (`tk_wup_tsk`/`tk_rel_wai`) and
  `task_manage.c:308`.

### 4.3 The minimal, byte-identity-safe cross-CPU wake design

The wake decision lives in `knl_make_ready` (`task.c:226-233`) — the single
choke point all 18 sites funnel through. The design adds ONE hook call at the end
of `knl_make_ready`:

```c
EXPORT void knl_make_ready( TCB *tcb )
{
    tcb->state = TS_READY;
    if ( knl_ready_queue_insert(&knl_ready_queue, tcb) ) {
        CUR_SCHEDTSK = tcb;
        knl_dispatch_request();
    }
    knl_smp_wake_hook(tcb);     /* NEW — empty macro when SMP off */
}
```

`knl_smp_wake_hook(tcb)` is a macro defined in `arch/aarch64/include/cpu_status.h`
(next to `BKL_ACQUIRE`), with the established two-arm gating:

```c
#ifdef SMP_SELFTEST
extern void knl_smp_wake(TCB *tcb);
#define knl_smp_wake_hook(tcb)  knl_smp_wake(tcb)
#else
#define knl_smp_wake_hook(tcb)  /* empty: no cross-CPU wake on a uniprocessor */
#endif
```

and a fallback `#define knl_smp_wake_hook(tcb)` (empty) in
`include/kernel/tkernel/task.h` next to the existing `_HAVE_CUR_SCHED_ACCESSORS_`
fallback block (`task.h:193-209`), so x86/linux/rl78 — which share
`kernel/common/task.c` but never define `SMP_SELFTEST` — compile unchanged.

`knl_smp_wake(TCB *tcb)` (in `smp.c`, `SMP_SELFTEST`-gated) is the **directed,
BKL-held** cross-CPU wake the cert needs:
- it is called with the BKL ALREADY HELD (every caller is inside
  `BEGIN/END_CRITICAL_SECTION`, §4.5);
- determine the target CPU. For the `[smp-secondary-wait]` cert the binding is
  directed: a task claimed for CPU 1 carries that affinity (§LENS-C). The minimal
  slice records the home CPU on the TCB-side cert state (or, for the cert,
  hardcodes target = 1 — the only secondary in the scenario), publishes
  `g_smpcpu[target].schedtsk = tcb` (under the held BKL), and
  `smp_send_reschedule(target)` — the SGI whose IRQ-return hits the ②.2b-i async
  switch on CPU 1, which re-dispatches `tcb`.
- **`-DSMP_NO_XWAKE`** compiles `knl_smp_wake` to skip the
  `smp_send_reschedule` → falsifier (§1.2).

> **The general policy is explicitly deferred.** "Scan all CPUs, pick the
> lowest-priority/right-affinity target, send only if outranked" is ②.3
> production-scheduler work. ②.2b-ii implements the **directed single-target
> wake** the cert needs and ledgers the general policy as deferred — mirroring how
> ②.1a proved the SGI MECHANISM, not the policy (`smp.c:965-981`).

### 4.4 The `[smp-secondary-wait]` harness

Gated `SMP_SECONDARY_WAIT`, building on `smp_prod.c`/`smp_async.c` machinery
(`smp_secwait.c`, new):

- **Half (i):** the secondary's real task `Bdly` calls `tk_dly_tsk(N_TICKS)`,
  recording `g_sec_slept=1` before and, after waking, `g_sec_woke_i=1` +
  `g_sec_wake_time = knl_current_time`. The driver on CPU 0 brings up the
  secondary (②.2a path), enables Half A's per-CPU tick, and — per §1.3 — keeps its
  OWN timer service out of the window so only CPU 1's tick can wake `Bdly`.
  **PASS (i):** `g_sec_slept==1 && g_sec_woke_i==1` and `knl_current_time` (driven
  by CPU 1's ticks) advanced ≥ `N_TICKS` between slept and woke.
- **Half (ii):** create a semaphore (count 0); the secondary's task `Bsem` calls
  `tk_wai_sem(sid, 1, TMO_FEVR)` (blocks forever — **no timeout, so ONLY a signal
  can wake it; a stray tick cannot**), recording `g_sem_blocked=1`. The driver on
  CPU 0 waits until `g_sem_blocked==1`, then `tk_sig_sem(sid, 1)`. The wake path
  (`tk_sig_sem` → `knl_wait_release_ok` → `knl_make_ready` →
  `knl_smp_wake(Bsem)` → `g_smpcpu[1].schedtsk=Bsem` + `smp_send_reschedule(1)` →
  CPU 1 async-redispatches `Bsem`). `Bsem` records `g_sem_woke=1`. **PASS (ii):**
  `g_sem_blocked==1 && g_sem_woke==1` and `smp_sgi_taken(1) >= 1` (an SGI was
  delivered — the cross-CPU doorbell rang).
- **Overall PASS:** both halves, watchdog-bounded → `SMP-SECONDARY-WAIT: PASS`.

Using `TMO_FEVR` for half (ii) is deliberate: it makes the semaphore waiter
**impossible to wake by any timer**, so the only thing that can pass half (ii) is
the real cross-CPU signal+IPI path. This is the half-(ii) analog of §1.3's
non-vacuity guarantee.

### 4.5 BKL-safety of the wake (discharged)

Every wake site is already inside `BEGIN/END_CRITICAL_SECTION`, which under ②.2a
acquires the BKL (`cpu_status.h:52`, `BKL_ACQUIRE()`): `tk_sig_sem`
(`semaphore.c:197-234`), `knl_timer_handler` (`timer.c:183-228`), `tk_wup_tsk`
(`task_sync.c:252-265`), `tk_dly_tsk`/`tk_slp_tsk` (`time_calls.c:143-149`,
`task_sync.c:216-231`). So `knl_smp_wake` runs with the BKL held — its
`g_smpcpu[target].schedtsk` write and `smp_send_reschedule` are BKL-serialized
against the target CPU's own scheduler mutations. **No new unlocked shared
state.** The only new cross-CPU action is the SGI, whose receiver path is the
already-certified ②.2b-i async switch (with its §5.4 guard).

---

## LENS A — BYTE-IDENTITY / CROWN RISK (the most dangerous)

**Goal:** the default build's `.text` sha stays `755a20fae2d9b741…`
(`docs/audit-trail.md:997`); x86/linux/rl78 (sharing `kernel/common/`) stay
byte-identical too. ②.2b-ii is the FIRST ② slice that must put a new statement
into a shared-core function (`knl_make_ready`). Enumerate every shared-core
function ②.2b-ii touches or relies on, and the EXACT gating that keeps N=1
byte-identical.

| # | Shared-core function (file) | What ②.2b-ii needs | Gating that preserves byte-identity | Risk |
|---|---|---|---|---|
| 1 | `knl_make_ready` (`task.c:226`) | append `knl_smp_wake_hook(tcb)` | macro → **empty** when `SMP_SELFTEST` off (`#define knl_smp_wake_hook(tcb)`); fallback empty define in `task.h` for non-aarch64 arches | **THE edit.** Must re-prove `.text` sha after. See risk note below. |
| 2 | `knl_make_wait` / `knl_make_wait_reltim` (`wait.c:192-232`) | unchanged — already uses `CUR_CTXTSK` | already covered by the ②.2a `CUR_*` macro (`cpu_status.h:25`); byte-identical at N=1 | none (no edit) |
| 3 | `knl_make_non_ready` / `knl_reschedule` (`task.c:253`, `task.h:268`) | unchanged — set `CUR_SCHEDTSK` of the calling CPU, which is correct for the BLOCK side | already covered by `CUR_*` macro | none (no edit) |
| 4 | `knl_timer_handler` (`timer.c:177-231`) | unchanged — runs on whichever CPU ticks, charges `CUR_CTXTSK`, RR-rotates `CUR_CTXTSK` | already `CUR_*`-based + `BEGIN/END_CRITICAL_SECTION` (BKL) at N>1; at N=1 resolves to the single global, byte-identical | none (no edit) |
| 5 | `timer_irq_handler` / `timer_init` (`tkdev_init.c:117-148`) | the secondary needs an equivalent — but Half A **replicates** it header-light in an SMP TU, does **NOT** edit it | secondary's CNTP/PPI live in `smp.c` (`SMP_SELFTEST`); `tkdev_init.c` untouched | none (no edit) |
| 6 | `tk_dly_tsk` / `tk_slp_tsk` / `tk_wai_sem` / `tk_sig_sem` (time_calls/task_sync/semaphore) | unchanged — they already funnel wakes through `knl_make_ready` (#1) and `CUR_*` | covered by #1's hook + `CUR_*`; no per-site edit | none (no edit) |

**The single shared-core EDIT is item #1 — one line in `knl_make_ready`.** The
precise gating that MUST hold for byte-identity:

1. `knl_smp_wake_hook(tcb)` MUST `#define` to **literally nothing** (not an empty
   inline that takes `tcb`) when `SMP_SELFTEST` is undefined. An empty
   object-like-with-args macro expands to whitespace, so the C compiler sees the
   identical token stream `knl_make_ready` had before the edit *minus a
   statement that produces no tokens* → identical AST → identical `.text`. This is
   the SAME mechanism that made `CUR_SCHEDTSK = tcb` and `BKL_ACQUIRE()`
   byte-identical (`smp_percpu.h:18-23`: "SMP-off macros expand to parenthesized
   identifiers → gcc compiles byte-identically").
2. The fallback empty define MUST be added to `include/kernel/tkernel/task.h`
   inside the existing `#ifndef BKL_ACQUIRE` style block (`task.h:206-209`), so
   the non-aarch64 arches (whose `cpu_status.h` predates ②.2 and never sets
   `SMP_SELFTEST`) see the empty macro and compile `kernel/common/task.c`
   unchanged. **Without this, x86/linux/rl78 fail to compile** (undefined
   `knl_smp_wake_hook`) — a build break, not just a sha drift.
3. **The implementer MUST re-run the `[smp-uniproc-semantics]` default-`.text`
   byte-identity guard (`run_smp2.sh:82-103`) AFTER editing `knl_make_ready`** and
   confirm the sha is still `755a20fae2d9b741…`. This is the one slice where that
   check could legitimately go red from a shared-core edit, so it is the gate.

**HEADLINE RISK (can it be made byte-identical at all?):** YES, by the
empty-macro mechanism above — *provided the macro genuinely expands to nothing in
the non-SMP build.* The one way it could FAIL: if an implementer writes the hook
as an `inline` function call `knl_smp_wake_hook(tcb);` that, even when the body is
empty `{}`, the compiler is allowed to (and at `-O1` with `-ffp-contract=off`,
the project's pinned recipe per MEMORY.md `Salty bug saga`, generally does NOT,
but is not guaranteed to) emit a register move to set up the argument before
eliding the call. **The mandate is therefore: use a preprocessor macro that
expands to EMPTY, never an empty inline that receives `tcb`.** If for any reason
the empty-macro form cannot be used (e.g. a future refactor needs the hook to read
shared state unconditionally), **byte-identity is broken and the slice must STOP
and escalate to the commander** — do not ship a moved `.text` and call it "close
enough." This is the project's crown constraint
(`smp-2-production-scheduler-plan.md §0`, `smp_percpu.h:17-31`).

**Note on the §1.3 non-vacuity witness (why we avoid a shared-core counter):** the
tempting way to prove "CPU 1's own tick fired" is to bump a counter inside
`knl_timer_handler` or `timer_irq_handler`. Both are shared core / shared
ELF-linked, so a `g_sec_tick_count[me]++` there would be a SECOND shared-core
edit and a second byte-identity risk. **This plan deliberately uses
`knl_current_time` advance as the positive witness instead** (it advances only
because a tick ran, `timer.c:184`), so Half A's non-vacuity needs **zero**
shared-core edits. Item #1 stays the ONLY shared-core touch.

---

## LENS B — DEADLOCK / RACE

Cross-CPU wake + per-CPU timer IRQ + the BKL is a deadlock surface. Enumerate
each hazard and name the guard, or mark it open.

**(a) CPU A holds the BKL and IPIs CPU B; B's IRQ tries to take the BKL.**
This is exactly the ②.2b `[smp-no-deadlock]` scenario. When `knl_smp_wake` (run by
CPU A under the held BKL, §4.5) `smp_send_reschedule(B)`s, the SGI arrives on B.
B's `_vec_el1_irq` → `smp_resched_sgi_handler` sets `g_resched_pending[B]`
(`smp.c:337-345`, no BKL needed) → the ②.2b-i hook `smp_irq_need_resched()` runs.
**Guard:** clause (5), the §5.4 BKL-held guard (`smp.c:632`): if B's interrupted
context held the BKL it DEFERS the switch. But here B is *parked* (not holding the
BKL), and A holds it — so the question is whether B's hook tries to ACQUIRE the
BKL. It does **not**: `smp_irq_need_resched` only READS `g_bkl_owner` with a `dmb`
(`smp.c:632`), it never calls `bkl_acquire`. The actual re-dispatch
(`knl_dispatch` from the hook, `cpu_support.S:386`) also does not acquire the BKL.
So while A holds the BKL, B can take the SGI, set pending, and — if it is NOT
itself mid-critical-section — switch; if A is still in `knl_smp_wake`, B's switch
to `tcb` is fine because `tcb`'s eventual kernel re-entry will `bkl_acquire` and
*spin* (via `wfe`, `smp.c:462-475`) until A releases — **bounded wait, not
deadlock**, because A is not waiting on B. **GUARDED** (reuses the certified ②.2b
guard + the BKL's recursive owner discipline). The `-DSMP_NO_BKL_GUARD` falsifier
from ②.2b already covers the symmetric case (a BKL-holder being switched away);
②.2b-ii adds no new BKL-acquire in interrupt context.

**(b) A timer IRQ on the secondary fires mid-WAIT-queue-manipulation while the BKL
is held by that same secondary.** When CPU 1 is *inside* `tk_dly_tsk`'s
`BEGIN/END_CRITICAL_SECTION` (BKL held by CPU 1, IRQ masked by `disint()` —
`cpu_status.h:52` masks I+F before `BKL_ACQUIRE`), the secondary's own PPI 30
**cannot fire** because IRQ is masked for the whole section. So a tick cannot
interrupt CPU 1's own wait-queue manipulation. **GUARDED** by the existing
`disint()` at section entry — the same protection the uniprocessor relies on. The
ONLY window where CPU 1 takes its tick is when it is running task code with IRQ
unmasked (i.e. not in a critical section), at which point `knl_timer_handler`
itself re-enters `BEGIN_CRITICAL_SECTION` and re-takes the BKL recursively (the
BKL is recursive: owner-CPU + depth, `smp.c:452-460,520-536`) — no self-deadlock.
**Cross-check the §5.4 guard interaction:** if CPU 1's tick fires while CPU 1
holds the BKL (e.g. a nested case), `smp_irq_need_resched` clause (5) sees
`g_bkl_owner == me` and DEFERS the async switch (`smp.c:632`) — correct, the
deferred reschedule retries after release. **GUARDED.**

**(c) Lost-wakeup: CPU A signals a sem just as CPU B's task transitions into
WAIT.** B's `tk_wai_sem` decides to block and B's `tk_sig_sem`-from-A both run
under `BEGIN/END_CRITICAL_SECTION` → **both serialize on the BKL** (`cpu_status.h:52`).
So either: (1) A's signal completes first → B, on entering the critical section,
re-checks `semcb->semcnt` (`semaphore.c:331-333`) and does NOT block (gets the
count); or (2) B blocks first (queues on `semcb->wait_queue`, sets `TS_WAIT`,
switches off) → A's later signal walks the wait queue (`semaphore.c:211-231`),
finds B, `knl_wait_release_ok(B)` → `knl_smp_wake(B)` IPIs CPU 1. **There is no
interleaving where A signals "between" B's check and B's enqueue, because the BKL
makes the check-and-enqueue atomic w.r.t. A.** This is precisely why the WAIT path
was already declared BKL-safe (§4.5). **GUARDED by the BKL serialization** — the
classic lost-wakeup is structurally impossible while the whole WAIT/SIGNAL path is
one big lock. **The residual subtlety to watch:** the IPI in `knl_smp_wake` is
sent while A holds the BKL, but the SGI is asynchronous — if B's task had ALREADY
been re-dispatched by B's own path between the `knl_make_ready` and the SGI
arriving, the SGI is a harmless spurious reschedule (the ②.2b-i hook's clause (2)
`ctxtsk == schedtsk` → returns 0, `smp.c:647`). **No lost or double wake.**

**(d) [OPEN RISK to watch] `knl_taskindp` is still a single GLOBAL** (`W` signed
int, `cpu_init.c`, read by `smp_irq_need_resched` clause (4), `smp.c:637`). The
secondary's tick handler `knl_timer_handler_startup` increments
`knl_taskindp` globally (`cpu_support.S:224-234`) around `knl_timer_handler`.
Under genuine concurrent ticks on CPU 0 and CPU 1, both increment/decrement the
SAME global — a race on a non-atomic `W`. For the ②.2b-ii cert the secondary's
tick and the boot CPU's tick are serialized by the BKL *inside* `knl_timer_handler`
(`timer.c:183`), but the `knl_taskindp++` in the asm startup shim
(`cpu_support.S:224-227`) is **OUTSIDE** the BKL (it brackets the call). **This is
a real residual** flagged by ②.2b-i (`smp-2b-async-preempt-plan.md:136-138`):
> *"②.2b must either (a) make `knl_taskindp` per-CPU too … or (b) prove the cert
> never enters task-independent context on the secondary and ledger the global as
> a `[live]`-deferred sharpening."*

**②.2b-ii's call:** the half-(i)/(ii) cert tasks are plain task context; the
window where both CPUs run their timer startup shim concurrently with the
`knl_taskindp` bracket unlocked IS reachable once BOTH CPUs tick (Half A enables
the secondary tick). **The conservative fix this plan recommends: per-CPU-ize
`knl_taskindp`** (small, mirrors the `dispatch_disabled` per-CPU work,
`smp_percpu.h:67`) — OR, if scoping tight, mask the secondary's tick during the
measurement window so the two startup shims never overlap, and **ledger the global
`knl_taskindp` as an explicit `[live]`-deferred sharpening with a falsifiable
note.** The auditor MUST NOT let this slide silently (MEMORY.md
`feedback_cert_must_cover_all_paths`). **Marked OPEN; the implementer must pick
(a) or (b) and the auditor must verify the chosen path covers the concurrent-tick
case.**

---

## LENS C — WAIT-STATE CORRECTNESS

**Who becomes `CUR_SCHEDTSK` for CPU 1 when a task on CPU 1 blocks?** When CPU 1's
task calls `tk_dly_tsk`/`tk_wai_sem`, `knl_make_non_ready(CUR_CTXTSK)`
(`task.c:253-260`) runs on CPU 1, so `CUR_SCHEDTSK` resolves to
`g_smpcpu[1].schedtsk` and is set to `knl_ready_queue_top(...)` — **the next
runnable task as seen by CPU 1.** If the shared ready queue has another runnable
task, CPU 1 switches to it; if not, `CUR_SCHEDTSK` becomes whatever
`knl_ready_queue_top` returns (possibly NULL → CPU 1 idles in `.Lidle`,
`cpu_support.S:182-186`). **The per-CPU schedtsk IS wired through the WAIT-exit
path** because the BLOCK side runs `END_CRITICAL_SECTION` → `knl_dispatch` on
CPU 1, all `CUR_*`-resolved to CPU 1's slot. **Correct.**

> **Subtlety the implementer must confirm:** the SHARED `knl_ready_queue` means
> CPU 0 and CPU 1 pull from ONE pool. When CPU 1 blocks and re-selects
> `knl_ready_queue_top`, it could pick a task CPU 0 also intends to run. Under the
> BKL this is serialized (only one CPU re-selects at a time), but the cert's
> claimed tasks are pulled OUT of the ready queue (`async_claim_for_secondary`,
> `smp_async.c:222-228`) precisely to avoid double-dispatch. The
> `[smp-secondary-wait]` harness must claim its tasks the same way so the
> re-selection on block is deterministic.

**When CPU 0 wakes the task, does it resume on CPU 1 or get stolen by CPU 0?**
This is the **task-affinity / migration** question, and the design makes it
**explicit and PINNED to no-migration for the cert:** `knl_smp_wake(tcb)` publishes
`g_smpcpu[1].schedtsk = tcb` (the task's home CPU) and IPIs **CPU 1**, NOT CPU 0.
The woken task is re-dispatched on CPU 1. CPU 0, after `tk_sig_sem` returns, does
NOT switch to `tcb` because `knl_make_ready` set `g_smpcpu[0].schedtsk = tcb`
(`task.c:230`) — wait, this is the crux: **`knl_make_ready` sets the CALLING CPU's
schedtsk to the woken task.** So after the signal, CPU 0's `END_CRITICAL_SECTION`
sees `CUR_CTXTSK(CPU0) != CUR_SCHEDTSK(CPU0)==tcb` and would `knl_dispatch` →
**CPU 0 would steal `tcb`!** This is the migration hazard.

**The design's resolution (MUST be implemented exactly):** in `knl_smp_wake`,
**after** `knl_make_ready` has set `g_smpcpu[0].schedtsk = tcb`, the hook (running
under the BKL, on CPU 0) must **restore CPU 0's schedtsk to CPU 0's correct task**
(re-run `knl_reschedule` semantics for CPU 0 *excluding* the home-pinned `tcb`) and
publish `g_smpcpu[1].schedtsk = tcb` + IPI CPU 1. Concretely:
- `knl_make_ready` sets `CUR_SCHEDTSK`(=CPU 0's) `= tcb` only **if `tcb` outranks
  CPU 0's current top** (`knl_ready_queue_insert` returns true, `task.c:229`). For
  the cert, the secondary's `Bsem` is lower priority than CPU 0's initial task, so
  `knl_ready_queue_insert` returns FALSE and CPU 0's schedtsk is **untouched** —
  no steal. **The cert tasks are deliberately chosen lower-prio than CPU 0's
  driver task** (itskpri 8, like `smp_async.c:255`) so the steal path is not even
  entered. **PIN: the cert uses a low-prio secondary task so `knl_make_ready` does
  not redirect CPU 0's schedtsk; `knl_smp_wake` then only needs to set CPU 1's
  schedtsk + IPI.**
- **The general case (a HIGH-prio task woken that outranks CPU 0's current) is
  where real migration policy is needed — and that is explicitly ②.3 deferred.**
  ②.2b-ii's `knl_smp_wake` handles the directed low-prio cert case; the auditor
  must confirm the cert task priorities keep CPU 0 out of the steal path, and the
  general affinity policy is ledgered.

**Which CPU's tick scans the single global `knl_timer_queue`, and is it
BKL-protected against concurrent scan+insert?** Under Half A (T1), **BOTH** CPUs'
ticks scan the SAME `knl_timer_queue` (`timer.c:199-216`). Concurrent
scan-by-CPU-1 vs insert-by-CPU-0 (`knl_enqueue_tmeb`, `timer.c:93-108`, called
from `knl_timer_insert*` inside another task's `tk_dly_tsk`) IS a shared-structure
race — **but it is BKL-protected:** `knl_timer_handler` wraps its whole walk in
`BEGIN/END_CRITICAL_SECTION` (`timer.c:183,228`), and every `knl_timer_insert*`
runs inside the blocking syscall's `BEGIN/END_CRITICAL_SECTION` (§4.5). So scan and
insert serialize on the BKL. **GUARDED — no concurrent scan+insert.** The one thing
to verify: `knl_timer_handler`'s `knl_clear_hw_timer_interrupt` /
`knl_end_of_hw_timer_interrupt` (`timer.c:181,230`) are OUTSIDE the critical
section; on aarch64 these are the CNTP reload (per-CPU register) and are
per-CPU-safe (each CPU clears its OWN timer condition by writing its local
`CNTP_TVAL_EL0`, §3.1) — no shared-state touch outside the BKL.

---

## 5. The safety boundary (how the secondary timer avoids perturbing N=1)

1. **N=1 / default build BYTE-IDENTICAL.** The ONE shared-core edit
   (`knl_make_ready` + the empty `knl_smp_wake_hook` macro) compiles to identical
   `.text` at N=1 (LENS A item #1). Everything else is `SMP_SELFTEST`-gated
   (`smp.c`/`smp_secwait.c`). `tkdev_init.c`/`timer.c`/`wait.c`/`semaphore.c` are
   **not edited**. The `[smp-uniproc-semantics]` default-`.text` guard
   (`run_smp2.sh:82-103`) MUST stay green at sha `755a20fae2d9b741…`.
2. **The boot CPU's tick/WAIT at N=1 unchanged.** No secondary exists; `timer_init`
   /`gic_init` run as today; `knl_timer_handler` walks the queue as today; the
   `knl_smp_wake_hook` is the empty macro. The cross-CPU wake only fires when a
   secondary actually runs.
3. **The secondary's timer enable touches ONLY per-CPU-banked hardware**
   (`CNTP_*_EL0`, the banked `GICD_ISENABLER0` PPI 30 bit, `GICC_*`) — it cannot
   perturb CPU 0's timer/GIC, and at N=1 it never executes. The §5.3 GICv2
   PPI-banking claim is inherited from ②.1a and MUST be re-confirmed against the
   running QEMU (not the spec).

---

## 6. BYTE-IDENTITY STRATEGY — gating for EVERY shared-core touch (explicit)

| Shared-core artifact | Edited? | Gating | How byte-identity is preserved at N=1 |
|---|---|---|---|
| `kernel/common/task.c` `knl_make_ready` | **YES (1 line)** | `knl_smp_wake_hook(tcb)`, `#define`d EMPTY when `!SMP_SELFTEST` (`cpu_status.h`) + empty fallback in `task.h` for other arches | empty object-like macro → zero tokens → identical AST → identical `.text`; **re-prove sha `755a20…` after edit** |
| `kernel/common/wait.c`, `timer.c`, `semaphore.c`, `task_sync.c`, `time_calls.c` | **NO** | already `CUR_*`-macro + `BEGIN/END_CRITICAL_SECTION` from ②.2a | unchanged source → unchanged `.text` |
| `arch/aarch64/tkdev_init.c` (`timer_init`/`timer_irq_handler`) | **NO** | secondary CNTP replicated header-light in `smp.c` | unchanged |
| `arch/aarch64/cpu_support.S` (`_vec_el1_irq`, `.Ldispatch_loop`) | **NO** (reuses ②.2b-i hook) | the async hook is already `#ifdef SMP_SELFTEST` | unchanged |
| `include/kernel/tkernel/task.h` | **YES (fallback define)** | add `#ifndef knl_smp_wake_hook … #define …(empty)` next to the `BKL_ACQUIRE` fallback (`task.h:206-209`) | empty macro for non-aarch64 → those arches compile unchanged |
| `arch/aarch64/include/cpu_status.h` | **YES (macro def)** | the two-arm `#ifdef SMP_SELFTEST` define | the `!SMP_SELFTEST` arm is empty → aarch64 default build unchanged |

**The acceptance gate:** the `[smp-uniproc-semantics]` harness re-run yields
default `.text` sha `755a20fae2d9b741…` AND all non-aarch64 arches still build.
This is mandatory and is the FIRST thing the auditor checks.

---

## 7. Honest scope / deferral

1. **QEMU green is NOT a hardware green.** QEMU TCG models memory strongly and may
   MASK a missing `dsb ish` / SGI-timing / tick-latency race. A green
   `[smp-secondary-wait]` proves the secondary timer + cross-CPU wake are
   correctly plumbed and load-bearing (block + self-tick wake + cross-CPU
   signal wake); it does NOT prove barrier discipline or real interrupt latency on
   weakly-ordered silicon. The teeth are only `[live]` on RPi3.
2. **RPi3 is NOT GICv2.** The BCM2837 ARM Local Interrupt Controller
   (`tkdev_init.c:29-111`) has no `GICD_SGIR` and a different timer-INTCTL path.
   The `[live]` RPi3 port needs the BCM2837 per-core mailbox IPI (for
   `smp_send_reschedule`) and the per-core `CORE_n_TIMER_INTCTL` routing (for the
   secondary tick) under a `BOARD_RPI3` `#ifdef`, mirroring the existing fork.
   Option T2 (boot-CPU-ticks-all) is the recorded fallback if RPi3 PPI routing is
   problematic. **Deferred.**
3. **Directed wake, not general policy.** `knl_smp_wake` implements the directed
   single-target wake the cert needs (target = the secondary; the cert task is
   low-prio so CPU 0 is not in the steal path). The general "scan all CPUs / pick
   the right-affinity, outranked target / migrate a high-prio woken task" policy
   is ②.3 production-scheduler work. **Deferred + ledgered.**
4. **Task affinity / migration is PINNED to no-migration for the cert** (the woken
   task resumes on its home CPU 1). True migration (a high-prio woken task
   stealing onto the lowest-priority CPU) is ②.3. **Deferred.**
5. **`knl_taskindp` global** (LENS B(d)) — either per-CPU-ize it (recommended) or
   prove + ledger the cert never overlaps the two CPUs' task-independent brackets.
   **Must be resolved this slice, not silently deferred.**
6. **`[smp-one-mind]` crown is ②.2c, not here.** ②.2b-ii adds no mind math; its
   only crown obligation is the negative one: don't regress default byte-identity,
   don't corrupt kernel state under a real cross-CPU wake.

---

## 8. OPEN RISKS the implementer must watch (checklist)

1. **[BIGGEST] `knl_make_ready` `.text` drift.** The `knl_smp_wake_hook` MUST be an
   empty PREPROCESSOR macro (never an empty inline taking `tcb`). Re-prove sha
   `755a20fae2d9b741…` after the edit. If it moves, STOP and escalate — do not
   ship a moved `.text`.
2. **Non-aarch64 build break.** Add the empty `knl_smp_wake_hook` fallback to
   `task.h` or x86/linux/rl78 won't compile `task.c`.
3. **Cert non-vacuity (§1.3).** In `-DSMP_NO_SEC_TIMER`, half (i) MUST hang. Keep
   CPU 0's timer service out of the half-(i) window (physically remove the
   alternative waker) and gate PASS on `knl_current_time` advancing (driven only
   by CPU 1's tick). Confirm against running QEMU that the falsifier truly times
   out.
4. **Migration steal (LENS C).** Keep the cert's secondary tasks LOWER priority
   than CPU 0's driver task so `knl_make_ready` does not redirect CPU 0's
   schedtsk. Verify CPU 0 never runs `Bsem`/`Bdly`.
5. **`knl_taskindp` concurrent-tick race (LENS B(d)).** Per-CPU-ize it OR mask the
   overlap + ledger. Auditor must verify the chosen path.
6. **GICv2 PPI-banking (§5.3).** Confirm against the running QEMU that enabling
   CPU 1's PPI 30 in its banked `GICD_ISENABLER0` does not alter CPU 0's PPI 30.
7. **`knl_intvec[30]` shared handler.** The secondary reuses the GLOBAL
   `timer_irq_handler` (registered once on CPU 0, `tkdev_init.c:160`). Confirm it
   is re-entrant across CPUs under the BKL — it reads `CNTFRQ`/writes local
   `CNTP_TVAL` (per-CPU) and calls `knl_timer_handler` (BKL-wrapped). Should be
   safe; verify no static non-BKL state in the path.
8. **SGI vs already-redispatched (LENS B(c)).** A spurious cross-CPU SGI (task
   already running) is harmless via clause (2) `ctxtsk==schedtsk`. Confirm no
   double-wake.
9. **Per-wave separation (MEMORY.md `feedback_development_method_is_the_life`):**
   implementer ≠ auditor ≠ commander. The auditor independently re-derives the
   byte-identity gate (§6) and the non-vacuity (§1.3), and confirms both
   falsifiers go RED.

---

## 9. Cert + falsifier summary (crisp)

- **`[smp-secondary-wait]`:** (i) a CPU-1 task `tk_dly_tsk`s and is woken by
  CPU 1's OWN tick (`knl_current_time` advanced by CPU 1; CPU 0's timer service
  held out of the window); (ii) a CPU-1 task `tk_wai_sem(…, TMO_FEVR)` blocks and
  is woken by CPU 0's `tk_sig_sem` → `knl_smp_wake` → SGI → CPU 1 re-dispatch
  (`smp_sgi_taken(1) >= 1`). Print `SMP-SECONDARY-WAIT: PASS`.
- **Falsifier `-DSMP_NO_SEC_TIMER`** → secondary CNTP unprogrammed → half (i)
  hangs → `FAIL` (proves the secondary timer is load-bearing).
- **Falsifier `-DSMP_NO_XWAKE`** → `smp_send_reschedule` suppressed in
  `knl_smp_wake` → half (ii) waiter never wakes → `FAIL` (proves the cross-CPU IPI
  is load-bearing).
- **Gating:** `smp_secwait.o` in `SMP_CERT_EXCLUDE`, linked only under
  `-DSMP_SECONDARY_WAIT` (implies `SMP_SELFTEST`). Default ELF / `.text`
  (`755a20fae2d9b741…`) unchanged.
- **The single byte-identity gate:** default `.text` sha unchanged AFTER the
  `knl_make_ready` one-line edit; non-aarch64 arches still build.
