# ②.2 — converting the production T-Kernel scheduler to true SMP (cert-first, multi-wave): the one mind under real concurrency

**Status: DESIGN PLAN** on trunk `fd4900a9` (②.0 + ②.1a merged + audited; `git log`: `fd4900a9 backlog: ②.1a SHIPPED+audited (p-kernel's first IPI on trunk); ②.1b/②.2 next`). Read-only on code; no implementation in this wave. This plan makes ②.2 **READY and de-risked** — it does **not** start it. ②.2 is the **most invasive change in the repo** (it changes the real scheduler) and is a **本丸-level decision awaiting mk_pino's go-ahead**, **per-wave**, via **separate impl→audit cycles** (implementer ≠ auditor ≠ commander; the development METHOD is the project's immune system). All file:line grounding below is against the shared checkout at `fd4900a9`.

mk_pino's stated END GOAL: *"最終的に2番にしたい"* — turn the T-Kernel **UNIPROCESSOR** scheduler into a real **symmetric-multiprocessing** kernel where N cores each run schedulable T-Kernel tasks. ②.0 built the BKL + per-CPU sandbox; ②.1a built the GIC SGI IPI. **②.2 is the heart of the goal: it graduates the sandbox into the real scheduler** — per-CPU-izing `knl_ctxtsk`/`knl_schedtsk`, making the production dispatcher run real T-Kernel TCBs on N CPUs under the BKL, adding a TRUE asynchronous register-context preempt (the SGI handler that actually *switches context*, not just sets a flag), and proving the crown: **the mind computes the same bits whether the kernel is uniprocessor or SMP.**

---

## 0. THE BIG HONESTY SHIFT — lead with this (the byte-identity guarantee MOVES)

**②.0 and ②.1a kept the shipped uniprocessor kernel BYTE-IDENTICAL.** They could, because every SMP symbol is compiled ONLY under `-DSMP_SELFTEST` (`smp.c:73`, `cpu_support.S:382`, `start.S:282`); with no flag, `smp.c` is an empty object, the production dispatcher `.Ldispatch_loop` (`cpu_support.S:90-124`) is byte-untouched, and `task.c`'s globals `knl_ctxtsk`/`knl_schedtsk` (`task.c:62-63`) are byte-untouched. The default `make` build carries NONE of ②.0/②.1a — the Makefile says so verbatim: *"The DEFAULT build (no -DSMP_SELFTEST) is BYTE-IDENTICAL to before"* (`boot/aarch64/Makefile:459`). That was the whole safety story for two waves: the SMP code lived in a gated sandbox over `struct smp_task` stand-ins (`smp.c:409-414`), never the real scheduler.

**②.2 CANNOT keep that guarantee. It changes the real scheduler.** To run real T-Kernel tasks on N CPUs, ②.2 must:
- make `knl_ctxtsk`/`knl_schedtsk` per-CPU (touching ~283 reader/writer sites; §2),
- redefine `BEGIN_CRITICAL_SECTION`/`END_CRITICAL_SECTION` (`arch/aarch64/include/cpu_status.h:17-24`) to acquire/release the BKL,
- make the production dispatcher (`cpu_support.S:48-124`) and the timer/IRQ path (`cpu_support.S:151-170`, `:234-284`) SMP-aware.

Even if ②.2 stays gated behind `-DSMP_SELFTEST` for the *build*, the **moment N≥2 actually runs**, the scheduler's behavior is no longer "the uniprocessor kernel." So the crown guarantee **moves** from:

> ~~"the default build is byte-identical to before"~~ (②.0/②.1a)

to a **two-part guarantee**:

> **②.2-(a) `[smp-uniproc-semantics]`** — a single-CPU build/run of the ②.2 kernel **behaves like today's uniprocessor kernel**: same strict-priority task ordering, same round-robin, same dispatch-disable semantics. The BKL is a no-op contention-wise at N=1 (one CPU never contends with itself; the recursive owner-check `bkl_acquire` returns immediately, `smp.c:372-375`), and the per-CPU arrays have exactly one live entry (`g_smpcpu[0]`). N=1 is the regression guard: ②.2 must not change the scheduler the uniprocessor sees.
>
> **②.2-(b) `[smp-one-mind]` (THE CROWN)** — the **mind's OUTPUT is BYTE-IDENTICAL** whether the same fixed-seed mind forward runs under N=1 or under N=2+ CPUs actually scheduling. This is the bit that says "② did not split the mind."

**Why this shift is honest and not a retreat:** the crown was NEVER "kernel scheduling traces are byte-identical" — scheduling order is *allowed* to differ (it already does on the hosted port under SIGALRM jitter; `full-smp-plan.md:20`). The crown is **byte-identity of the MIND'S OUTPUT**. ②.0/②.1a achieved it trivially by not touching the scheduler at all. ②.2 achieves it the hard way: by proving that a real concurrent scheduler **does not corrupt or reorder** the mind's math. The guarantee got *stronger* in what it proves (real concurrency is safe) and *narrower* in what it claims (mind output + N=1 semantics, not literal build byte-identity). **State this in every ②.2 cert and commit message: ②.2 is the first wave where the shipped kernel is not byte-identical; the guarantee is now mind-output-identity + uniproc-semantics-preservation.**

---

## 1. The determinism threat model for the PRODUCTION scheduler (extends `full-smp-plan.md` §1)

`full-smp-plan.md` §1 built the threat model abstractly (the ready queue, allocator, object tables *will* be raced). ②.2 makes it concrete: the **real** `knl_ready_queue` (`ready_queue.h:52`), the **real** allocator `knl_Imalloc` (`memory.c:275`), the **real** object tables, **and the real mind buffers** are now raced by N CPUs running real tasks. We re-state every risk in the §1 dichotomy — **CORRECTNESS race** (corruption; cured by the BKL) vs **DETERMINISM race** (could change mind output bits; cured by serialize/canonicalize/prove-already-safe) — and we add the one surface ②.0/②.1a never had to think about: **the mind's own module-static buffers** (`rw[]`/`rc`, `r3_incontext.c:141,161`), now reachable from a real schedulable task on any CPU.

### 1.1 The BKL is the blanket — confirm every kernel critical section is under it

The dominant ②.2 problem is the textbook SMP one: on a uniprocessor, **"disable interrupts IS the lock"** — a critical section under `disint()`/`enaint()` (`cpu_status.h:17,24,30-31`) cannot be interrupted on the only CPU, so nothing races it. Under SMP, `disint()` masks only the **local** CPU's DAIF; a second CPU in the kernel races right through it. The **Big Kernel Lock** (already built and proven load-bearing in ②.0: `bkl_acquire`/`bkl_release`, `smp.c:367-391`, with the `[smp-mutual-exclusion]` cert + its `-DSMP_MUTEX_NOLOCK` falsifier, `smp.c:465-478`) makes "only one CPU is inside the kernel at a time" true again — restoring the exact uniprocessor invariant for kernel code while tasks run concurrently in compute.

**②.2's obligation: route EVERY kernel critical section through the BKL.** Because every subsystem already routes through two macro pairs, this is a near-single-point change — redefine the macros (`cpu_status.h:17,30`) to acquire `g_bkl` after `disint()` and release before `enaint()`. The table below confirms coverage; the "cert must cover ALL paths" lesson (MEMORY.md) means **the one bare-`DI` site that bypasses the macros must be hand-converted**:

| Shared kernel state | Today's "lock" | file:line | ②.2 cure (CORRECTNESS race) |
|---|---|---|---|
| Ready queue `knl_ready_queue` (bitmap + per-prio + `top_priority` + `klocktsk`) | `BEGIN_CRITICAL_SECTION` = `disint()`/`enaint()` | `ready_queue.h:44-50`; macros `cpu_status.h:17-24` | BKL via the macro redefine. (The §1.4 livelock — `bitsearch1` infinite loop on a broken bitmap/top_priority invariant — is the exact corruption this prevents.) |
| Scheduler globals `knl_ctxtsk`/`knl_schedtsk`/`knl_dispatch_disabled` | single global, dispatcher-owned | `task.c:62-63,55` | **per-CPU** (§2) — these are the uniprocessor assumption itself, not just locked |
| Allocator `knl_Imalloc`/`knl_Ifree` (FreeQue) | **bare `DI(imask)` — "Exclusive control by interrupt disable"** | `memory.c:275,303,364,384` | **MUST hand-convert** — these 4 sites bypass the macros (raw `DI`/`EI`, not `BEGIN_*`). Acquire `g_bkl` directly or wrap in `BEGIN_DISABLE_INTERRUPT`. Miss one → heap corruption → split mind. |
| Object tables (sem/mtx/evtflag/mbox/msgbuf/mempool/mempfix/rendezvous/deviceio wait queues) | `BEGIN_CRITICAL_SECTION` / `BEGIN_DISABLE_INTERRUPT` | eventflag.c, mailbox.c, mempool.c, mempfix.c, messagebuf.c, mutex.c, rendezvous.c, deviceio.c, semaphore.c, wait.c (all via the macros) | BKL via the macro redefine (covered automatically) |
| Kernel object lock `knl_LockOBJ`/`klocktsk` + the *"Multiple READY tasks with kernel lock do not exist at the same time"* invariant | `BEGIN_CRITICAL_SECTION` + the invariant | `klock.c`; invariant `ready_queue.h:38` | BKL makes it automatically correct (one CPU in the kernel ⇒ one task transitions through `klocktsk` at a time). Flag for the finer-lock wave §6/②.3, not ②.2. |
| Timer queue `knl_timer_queue` + 64-bit `knl_current_time` + RR slice | `BEGIN_CRITICAL_SECTION` in `knl_timer_handler` | `timer.c:177-231` (`BEGIN` `:183`, RR `:219-226`, `END` `:228`) | BKL via the macro redefine. **But the timer handler reads `knl_ctxtsk` (`timer.c:187-194,219`) — these become per-CPU (§2.4): each CPU's tick must charge ITS OWN running task and rotate based on ITS OWN ctxtsk.** |
| `fastlock`/`fastmlock` counters | `DI`/`EI` around `++`/`--`/`BTS`/`BR` | `fastlock.c`, `fastmlock.c` | BKL (or atomics in ②.3) |

**Verdict for §1.1: all CORRECTNESS races; the BKL covers every one EXCEPT the 4 bare-`DI` allocator sites (`memory.c:275,303,364,384`), which must be hand-converted.** None touch the mind's *math* — but a corrupted ready queue or heap absolutely produces a wrong/dead mind. The crown is at risk **INDIRECTLY (via corruption)**, and the BKL eliminates it wholesale.

### 1.2 The NEW surface ②.2 has that ②.0/②.1a did not: the mind's module-static buffers

This is the one genuinely new determinism finding for ②.2, because ②.0/②.1a never ran the *real mind* on a second CPU — they ran `struct smp_task` stand-ins (`smp.c:409-414`). ②.2's crown cert runs the **real** `r_forward` (`r3_incontext.c:192-258`), which computes over **module-static** buffers:

- `static float rw[R_NP];` — the weights (`r3_incontext.c:141`)
- `static R_TC rc;` — the forward activations/scratch (`r3_incontext.c:161`)

**These are SHARED, not per-task.** On a uniprocessor only one task ever ran `r_forward` at a time, so `rc` (the scratch) was effectively single-owner by construction. Under SMP, **if two CPUs both entered a mind forward concurrently, they would race `rc` → garbage activations → wrong/non-deterministic logits.** Classification:

- **`rc` (scratch): a CORRECTNESS + DETERMINISM race** if two CPUs run `r_forward` concurrently. **Cure for ②.2's crown cert: run the mind forward on exactly ONE CPU at a time** (the cert measures a *single* fixed-seed forward; it does not need two concurrent forwards). The other CPU(s) run unrelated busy tasks to prove the scheduler is genuinely concurrent, but only one touches the mind. The honest scope: **②.2 proves the SMP scheduler does not corrupt a single mind forward; it does NOT make two concurrent mind forwards safe** (that needs per-call scratch or a mind-lock — deferred, §6, and it is exactly the hosted-port's job where the teacher parallelizes, §7).
- **`rw` (weights): read-only during a forward.** `r_forward` only reads `rw[]`; training (`r_backward` + `rw -= lr*rg`) is a *separate* path. As long as the cert's forward runs while no task is training, `rw` is a stable read-only input → byte-identical. **②.2's crown cert must NOT interleave a forward with a training step on another CPU** (that is a real determinism race on `rw`, deferred with the mind-lock).

**This is the load-bearing honest narrowing of the crown:** `[smp-one-mind]` proves *one* mind forward is byte-identical under a concurrent scheduler. Concurrent *mind* operations (two forwards, or forward+train) need a mind-lock and are ②.3+/hosted-port work.

### 1.3 DETERMINISM races on the mind's append/merge/replay paths — already insulated (carried from `full-smp-plan.md` §1.3)

The §1.3 probe already established the mind's append/merge/replay is canonical and order-independent, and ②.2 inherits that verbatim (no re-derivation):
- **DMN engram ring**: deterministic stride append + largest-remainder with a fixed tiebreaker (`lm_consolidate.c`) — math-only, crown NOT at risk.
- **`gl_merge`/`rw[]` merge**: order-independent SUM modulo the ~1e-6 IEEE float-add asterisk (`gossip_learn.c`) — already a shipped, accepted property; ②.2's only obligation is to **not reorder the merge input array**, which it won't (the array is assembled from the node set, not from task-scheduling order).
- **`r_forward`**: pure math over `rw[]`+inputs, no global reads, no RNG (`r3_incontext.c:192-258`) — math-only.

**②.2's job is to NOT CORRUPT that determinism under a real concurrent scheduler** (via the BKL for kernel state + single-forward-at-a-time for the mind scratch), not to re-establish it.

### 1.4 The decisive precedent (carried from `full-smp-plan.md` §1.4)

The hosted port already hit ②.2's exact race and named it: `arch/linux/aarch64/preempt.c` documents `knl_timer_handler_startup` running CONCURRENTLY with task code, corrupting the ready/timer queues → a `bitsearch1` livelock in `knl_ready_queue_delete` once the bitmap/top_priority invariant breaks. The hosted fix *retreated* (pin the signal to one thread, `SIGEV_THREAD_ID`). **②.2 is the disciplined generalization that does NOT retreat: it makes the ready-queue access mutually exclusive across CPUs via the BKL.** The `[smp-one-mind]` and `[smp-no-deadlock]` falsifiers target exactly this corruption mode (§5).

### 1.5 What the BKL does NOT cover (state it honestly)

The BKL serializes **kernel entry**. It does **not** cover:
1. **A task spinning in userspace/compute reading shared mind state.** A task doing `r_forward` is NOT in the kernel — the BKL is not held during compute (by design; that's where the concurrency win is). So the mind's own buffers (`rc`, §1.2) are NOT protected by the BKL. This is why the crown cert runs ONE forward at a time, and why concurrent mind ops need a separate mind-lock (deferred).
2. **`g_resched_pending[]`** (`smp.c:198`) — per-CPU, one writer (the SGI handler on that CPU) + one reader (the dispatcher on that CPU) → needs only a `dmb`, correct without the BKL (already the case in ②.1a, `smp.c:242-250`).
3. **The cross-CPU barrier discipline** (`dsb ish` before the SGIR write, `smp.c:216`; the BKL's `dsb ish`, `smp.c:380,387`) — the BKL provides mutual exclusion but the *memory ordering* teeth are `[live]`-only on RPi3 (QEMU TCG models memory strongly; `smp.c:64-68`).

---

## 2. The per-CPU `knl_ctxtsk`/`knl_schedtsk` conversion — the largest mechanical diff in the repo

### 2.1 The site count (CORRECTED from the docs' "~166")

Grep on trunk `fd4900a9` (the docs quote "~166"; the real numbers are higher — enumerate honestly):

```
knl_ctxtsk    : 232 occurrences across kernel/ arch/ include/
knl_schedtsk  :  72 occurrences
combined      : 283 occurrences
```

Per-directory, the conversion targets are the **arch-agnostic `kernel/common/`** scheduler + object code (these are what an aarch64-only ②.2 must per-CPU-ize, because aarch64 compiles them):

| File | `knl_ctxtsk` | category |
|---|---|---|
| `kernel/common/rendezvous.c` | 23 | "current task" reads (the requesting task in a rendezvous) → per-CPU |
| `kernel/common/mutex.c` | 16 | "current task" (lock owner / waiter) → per-CPU |
| `kernel/common/deviceio.c` | 16 | "current task" → per-CPU |
| `kernel/common/wait.c` | 15 | "current task" (the task going to wait) → per-CPU |
| `kernel/common/messagebuf.c` | 11 | "current task" → per-CPU |
| `kernel/common/task_manage.c` | 10 | mixed (incl. `tk_get_tid` returning `knl_ctxtsk->tskid`, `:549`) → per-CPU |
| `kernel/common/timer.c` | 9 | **DANGEROUS** — the tick charges `knl_ctxtsk` time + RR-rotates on it (`:187-194,219`) → per-CPU (§2.4) |
| `kernel/common/klock.c` | 8 | "current task" (the task taking the kernel lock) → per-CPU |
| `kernel/common/task_sync.c` | 7 | "current task" → per-CPU |
| `kernel/common/mempool.c`, `eventflag.c`, `time_calls.c`, `semaphore.c`, `mempfix.c`, `mailbox.c`, `subsystem.c`, `misc_calls.c`, `device.c` | 2–5 each | "current task" reads → per-CPU |
| `kernel/common/task.c` | 2 | the def (`:62`) + init `knl_ctxtsk = knl_schedtsk = NULL` (`:102`) → per-CPU array init |
| `arch/aarch64/cpu_support.S` | 7 (+2 schedtsk) | **DANGEROUS** — the dispatcher save/restore (`:51-52,78-85,95-96`) → per-CPU (§2.3) |
| `arch/aarch64/include/cpu_status.h` | 3 (+1 schedtsk) | **DANGEROUS** — `END_CRITICAL_SECTION` dispatch test (`:19`), `in_indp`/`in_qtsk` (`:49,52`) → per-CPU |

Of the 140 `knl_ctxtsk` reads in `kernel/common/`, **almost all mean "the task running on THIS CPU"** (`get_tcb_self`, `knl_ctxtsk->`, time accounting, the requesting task of a syscall). There is exactly **one real production write** to `knl_ctxtsk` in C — the init `task.c:102` — every other write is the **asm dispatcher** (`cpu_support.S:52,85,96`). `knl_schedtsk` is written in `task.c:230,257` (`knl_make_ready`/`knl_make_non_ready`), `knl_reschedule` (`task.h:246`), and the dispatcher (`cpu_support.S:91`).

> **Honest scope note:** ②.2 is **aarch64 bare-metal only** (like ②.0/②.1a; `smp.c:70`). The `arch/x86/`, `arch/linux/{aarch64,x86_64}/`, `arch/rl78/` `knl_ctxtsk`/`knl_schedtsk` sites (the remaining ~120 of the 283) are **NOT touched by ②.2** — they keep the global. Because `kernel/common/` is shared across arches, the per-CPU accessor (§2.2) must be a **macro that compiles to the plain global when SMP is off** so x86/linux/rl78 builds are unaffected. This is the §6 "do not regress the other arches" obligation.

### 2.2 The conversion PATTERN (the recipe applied to every site)

The pattern, grounded in the two distinct meanings the docs identify:

**(A) `knl_ctxtsk` = "the task running on THIS cpu" → a per-CPU accessor.** Define accessor macros that, under SMP, resolve to `g_smpcpu[smp_this_cpu()].ctxtsk` (extend the existing per-CPU block, `smp.c:294-320`, whose `ctxtsk` field at offset 0 already exists), and under no-SMP resolve to the plain global (so x86/linux/rl78 + the N=1 uniproc path are byte-identical):

```c
/* in cpu_status.h or a new sched_percpu.h, included by task.h */
#ifdef SMP_SELFTEST
#  define CUR_CTXTSK    (g_smpcpu[smp_this_cpu()].ctxtsk)
#  define CUR_SCHEDTSK  (g_smpcpu[smp_this_cpu()].schedtsk)
#else
#  define CUR_CTXTSK    (knl_ctxtsk)      /* the plain global — byte-identical */
#  define CUR_SCHEDTSK  (knl_schedtsk)
#endif
```

Then mechanically rewrite each "current task" read `knl_ctxtsk` → `CUR_CTXTSK`. The 140 `kernel/common/` reads are almost all this case. Indexing is by `smp_this_cpu()` = MPIDR Aff0 (`smp.c:323`), the firmware-independent "which core am I."

**(B) `knl_schedtsk` = "the highest-priority ready task" → stays derived from the ONE shared ready queue, under the BKL.** `knl_schedtsk` is *not* fundamentally per-CPU state — it is "the next task the ready queue says should run." Under ②.2's **one global ready queue** (`full-smp-plan.md` §3.3 — NOT per-CPU run-queues yet), each CPU, when it dispatches, locks the BKL, pops the highest-priority *unclaimed* runnable task, and that becomes *its* `g_smpcpu[me].schedtsk`. So:
- `knl_reschedule` (`task.h:240-249`) keeps reading `knl_ready_queue_top` (`ready_queue.h:73`) but now, instead of setting one global `knl_schedtsk`, it must (i) set the appropriate CPU's `schedtsk` and (ii) **if that CPU is not the current one, `smp_send_reschedule(target)`** (the ②.1a IPI, `smp.c:205`). This is the "make `knl_reschedule` IPI-aware" work the ②.1a plan explicitly deferred to ②.2 (`smp-1-ipi-preempt-plan.md:273`).
- The dispatcher loads `g_smpcpu[me].schedtsk` not the global (§2.3).

**The dangerous sites (flag explicitly, audit independently):**
1. **The dispatcher** (`cpu_support.S:48-124`): saves to `knl_ctxtsk->tskctxb.ssp` (`:78-82`), clears `knl_ctxtsk` (`:52,85`), loads `knl_schedtsk` → `knl_ctxtsk` (`:91-96`). ②.2 must make all three per-CPU (§2.3). A bug here = a CPU saves another CPU's context = instant corruption.
2. **`END_CRITICAL_SECTION`** (`cpu_status.h:18-24`): the dispatch test `knl_ctxtsk != knl_schedtsk` (`:19`) must become `CUR_CTXTSK != CUR_SCHEDTSK` — i.e. "does THIS CPU need to switch." A bug = a CPU dispatches based on another CPU's state.
3. **The timer handler** (`timer.c:187-226`): charges time to `knl_ctxtsk` and RR-rotates on it — must be `CUR_CTXTSK` so each CPU's tick accounts ITS OWN task (§2.4).
4. **`in_indp()`/`in_ddsp()`/`in_qtsk()`** (`cpu_status.h:49-52`): all read `knl_ctxtsk` — must be `CUR_CTXTSK`.

**Also per-CPU** (the docs note these, grounded here): `knl_taskmode` and `knl_taskindp` (`sysinfo_depend.h:32-33`) — the dispatcher saves/restores `knl_taskmode` per task (`cpu_support.S:73-75,114-115`) and the timer asm bumps `knl_taskindp` (`cpu_support.S:157-167`). Both are inherently per-CPU live values; add them to `struct smp_cpu` (`smp.c:294`) alongside the existing fields. `knl_dispatch_disabled` (`task.c:55`) → per-CPU (a documented semantic narrowing: `tk_dis_dsp()` means "this task won't be preempted *on its CPU*", §6).

### 2.3 The dispatcher per-CPU-ization (the asm)

The production dispatcher `.Ldispatch_loop` (`cpu_support.S:90-124`) loads `=knl_ctxtsk`/`=knl_schedtsk` as global addresses. ②.2 replaces those with `&g_smpcpu[mpidr_aff0()].{ctxtsk,schedtsk}` — exactly the per-CPU load `smp_cur_tcb_load` (`cpu_support.S:402-410`) already demonstrates in the sandbox (`madd x0, x1, #SMPCPU_SIZE, =g_smpcpu`). The sandbox already proved the per-CPU current-task read works in asm indexed by MPIDR; ②.2 generalizes the *whole* dispatcher save/restore the same way. The per-CPU `.Lidle` (`cpu_support.S:120-124`) already exists (each CPU's `wfe`); ②.2 makes it wakeable by the reschedule SGI (an SGI is a `wfe` wake event once the CPU interface is enabled — exactly ②.1b's idle-wake mechanism, `smp-1-ipi-preempt-plan.md:269`).

### 2.4 The per-CPU timer tick

The tick drives preemption (`timer.c:177-231`; `knl_timer_handler_startup` asm `cpu_support.S:151-170`). Under SMP, **each CPU programs its own EL1 physical timer** (the timer is a per-CPU PPI id 30; `full-smp-plan.md` §5) and its handler charges `CUR_CTXTSK` and RR-rotates on it under the BKL. The timer queue `knl_timer_queue` + `knl_current_time` stay shared (under the BKL); only the per-task accounting + RR slice become per-CPU. **This is the highest-risk path** (the repo's recurring "aarch64 IRQ-path C-ABI trap" + the §1.4 hosted livelock both live here) — the `[smp-no-deadlock]` falsifier targets exactly the concurrent-timer-handler-vs-ready-queue corruption.

### 2.5 Staging the conversion (do NOT do all 283 sites at once)

The BKL means most readers are already serialized, so the per-CPU-ization can be **staged inside ②.2a**: first make the macros resolve to per-CPU but with **only the dispatcher + END_CRITICAL_SECTION + timer** converted (the 4 dangerous sites), keeping the rest as global reads that are *correct under the BKL because only one CPU is in the kernel*. Then convert the 140 `kernel/common/` "current task" reads to `CUR_CTXTSK` once a second CPU actually runs the production dispatcher. The "cert must cover ALL paths" lesson applies: the `[smp-one-mind]` + `[smp-uniproc-semantics]` certs are the enumeration guard — any missed site that reads another CPU's task shows up as a semantics regression (N=1 differs) or a mind-output difference.

---

## 3. True asynchronous register-context preempt — the SGI handler that SWITCHES context

### 3.1 What ②.1a does today vs what ②.2 must do

②.1a's SGI handler **only sets a flag** (`smp_resched_sgi_handler`, `smp.c:242-250`): `g_resched_pending[me] = 1`, then returns; the EOIR is done by the existing vector tail (`cpu_support.S:280`). The actual reschedule is **cooperative-at-a-checkpoint**: B's low-prio task polls `g_resched_pending[me]` at loop boundaries (`smp.c:663-695`) and re-selects under the BKL. The ②.1a plan is explicit this is the deferral: *"A TRUE asynchronous register-context preempt inside the SGI handler is the production context-switch work, DEFERRED to ②.2"* (`smp.c:617-620`; `smp-1-ipi-preempt-plan.md:251,273`).

**②.2 makes the preempt TRUE asynchronous:** when the SGI fires on CPU B while B is running task T (anywhere — mid-compute, not at a checkpoint), the IRQ-return path must (i) **save T's full register context** into `T->tskctxb.ssp` (the 112-byte frame the dispatcher saves, `cpu_support.S:65-85`), and (ii) **switch to `g_smpcpu[B].schedtsk`** — a real preemptive context switch from interrupt context. No cooperation, no polling.

### 3.2 The sketch (against the existing `knl_dispatch` save/restore)

The cleanest design **reuses the existing dispatcher** rather than writing a second context switch. The IRQ vector `_vec_el1_irq` (`cpu_support.S:234-284`) already does `save_caller_regs` (`:235`) on entry and `restore_caller_regs`+`eret` (`:283-284`) on exit. The reschedule SGI handler, instead of returning to `eret`, must redirect the return through the dispatcher. Two viable shapes:

**Option A — "request dispatch on IRQ return" (lower risk, matches the existing END_CRITICAL_SECTION model).** The SGI handler sets `g_resched_pending[me]` (as today). Then, in the IRQ-return tail (`cpu_support.S:271-284`), **before `restore_caller_regs`/`eret`**, check: if `g_resched_pending[me]` AND `CUR_CTXTSK != CUR_SCHEDTSK` AND `!CUR_dispatch_disabled` AND `!taskindp`, then instead of returning to the interrupted task, **call `knl_dispatch`** (`cpu_support.S:61`) which saves the interrupted context to `CUR_CTXTSK->ssp` and switches to `CUR_SCHEDTSK`. This is the **exact condition `END_CRITICAL_SECTION` already uses** (`cpu_status.h:18-22`) — ②.2 just adds the same check to the IRQ-return path. The interrupted task's full register state is on the IRQ stack frame (`save_caller_regs`); `knl_dispatch`'s save path captures the callee-saved + SP. **This is the standard T-Kernel preemption model** (dispatch-on-interrupt-return) generalized per-CPU; it is the lowest-risk way past the aarch64 IRQ-path C-ABI trap because it reuses the proven `knl_dispatch` save/restore rather than inventing a new one.

**Option B — full async switch inside the handler (higher risk, deferred unless A is insufficient).** The handler itself does the save/restore. This requires getting the full interrupted context (incl. the IRQ-saved caller regs) into the TCB frame, which means reconciling `save_caller_regs`'s frame with `knl_dispatch`'s 112-byte frame — exactly the kind of C-ABI/stack-layout reconciliation the repo's IRQ-path history warns about (the `_vec_el1_irq` reserved 16-byte IAR slot, `:240,274`, must not be clobbered). **Prefer A; only reach for B if a dispatch-on-return latency proves unacceptable.**

### 3.3 Why this is the hardest, most C-ABI-sensitive piece

The repo's standing rule (MEMORY.md): *"When input/timing doesn't work on aarch64, suspect the IRQ vector before the device. Two C-ABI traps recur."* The IRQ-return-path dispatch must: not clobber the vector's reserved IAR slot (`cpu_support.S:240,274`); EOIR **before** switching context (else the GIC won't deliver the next IRQ to the new task — EOIR at `:280` must run on the *old* context's behalf); ensure `knl_dispatch`'s 112-byte frame and the IRQ frame nest correctly so the interrupted task resumes exactly where it left off when it's later re-dispatched. **②.2b is the wave for this and only this**; its falsifier (§5) must prove a task is preempted *mid-compute* (not at a checkpoint), distinguishing it from ②.1a's cooperative version.

---

## 4. THE CROWN CERT `[smp-one-mind]` (cert-first)

### 4.1 What mind computation carries the crown

**The bare-metal R3 `r_forward`** (`r3_incontext.c:192-258`) — pure math over the module-static `rw[]`/`rc` (`:141,161`), no global scheduling reads, no RNG. It is the representative bare-metal mind computation: the big teacher LLM is hosted-only, but R3/dtr/MoE math is dual-compiled into the bare-metal kernel (per MEMORY.md wave-27, the mind's math runs in ring3 via `moe.c`+`dtr.c`; `r_forward` is the R3 in-context forward). **`[smp-one-mind]` runs `r_forward` with a fixed input/seed under the SMP kernel with N=2+ CPUs actually scheduling, and asserts the FNV-1a hash of its output (the `rc.probs` logits / the returned loss) is BYTE-IDENTICAL to the same forward under N=1 / today's uniprocessor kernel.**

The hash idiom already exists and is directly reusable: `ss6live_logit_hash` (FNV-1a over a logits buffer, `student_shell.c:689-696`: `h=1469598103934665603; for each byte: h^=b; h*=1099511628253`). The cert hashes `rc.probs` (the `R_VALV` softmax output, `r3_incontext.c:253-254`) or, more strongly, the full `rc` activation tensor.

### 4.2 The harness

1. **Uniprocessor reference:** build/run at N=1 (or today's pre-②.2 build), run `r_forward(fixed_key, fixed_val, fixed_label)`, FNV-1a hash `rc.probs` → `hash_uni`. Print `SMP-ONEMIND-UNI: <hash>`.
2. **SMP run:** build ②.2 with `-DSMP_SELFTEST`, boot under QEMU `-smp 4` with **N CPUs actually scheduling real tasks** (other CPUs run busy filler tasks so the scheduler is genuinely concurrent — proven by the `[smp-N-tasks-run]` per-CPU exec counters, `smp.c:527-539`). On ONE designated CPU, run the SAME fixed-seed `r_forward`, hash → `hash_smp`. Print `SMP-ONEMIND-SMP: <hash>`.
3. Assert `hash_smp == hash_uni` (and `memcmp(probs_uni, probs_smp)==0`). Print `SMP-ONEMIND: PASS/FAIL`. Grep harness (`tests/aarch64/run_smp2.sh`, modeled on `run_smp1.sh`).

**The crucial design constraint (from §1.2):** the mind forward runs on **exactly one CPU at a time** (the cert measures a single forward; concurrent forwards race `rc` and are out of scope). The other CPUs prove concurrency by running *non-mind* busy tasks. This is the honest, narrow, true claim: **a real concurrent scheduler does not perturb a single mind forward's bits.**

### 4.3 The FALSIFIER (MUST go RED)

A deliberately-racy ②.2 build where a shared mind buffer is touched without the BKL — concretely, **`-DSMP_ONEMIND_RACE`: let a second CPU's busy task scribble into `rc` (or run a second `r_forward`) concurrently with the cert's forward, WITHOUT serialization.** Under `-smp 4` the `rc` scratch race makes the activations garbage → `hash_smp != hash_uni` → `SMP-ONEMIND: FAIL`. This proves the cert actually guards determinism (it is not vacuously passing because the op is single-threaded anyway). A second falsifier variant — **`-DSMP_NO_RQLOCK`** (the §1.4 corruption: two CPUs race `knl_ready_queue_delete`) — proves the kernel-state-corruption path also breaks the mind (via a dead/wrong scheduler), and ties `[smp-one-mind]` to `[smp-no-deadlock]`.

### 4.4 QEMU-testable vs RPi3-`[live]`

| Sub-claim | QEMU `-smp 4` | RPi3 `[live]` |
|---|---|---|
| `r_forward` is byte-identical uniproc vs SMP (the partition/scratch/no-corruption determinism) | **YES** (full — this is what `[smp-one-mind]` proves on QEMU) | yes |
| The `-DSMP_ONEMIND_RACE` falsifier goes RED (a shared-buffer race perturbs the mind) | **YES** (load-bearing proven) | yes |
| The **cache-coherency / barrier discipline** (a missing `dsb ish`/SMPEN=0 doesn't silently corrupt the mind on weakly-ordered silicon) | **NO — QEMU TCG masks it** | **ONLY here** |

**Honest statement (MC-2 §4.4 / ②.0 `smp.c:64-68`, restated):** a QEMU `[smp-one-mind]` PASS proves the **scheduler did not reorder/corrupt the mind's math under real concurrency**; it does **NOT** prove the BKL/SGI barrier discipline on weakly-ordered silicon. **A QEMU green is NOT a hardware green.** The barrier sub-claim is `[live]`-only on RPi3 (and RPi3 uses the BCM2837 mailbox IPI, not GICD_SGIR — `smp-1-ipi-preempt-plan.md:233,250` — so the RPi3 `[live]` port needs the BCM2837 send path, a deferred follow-up).

### 4.5 `[smp-uniproc-semantics]` — the no-regression cert (the (a) half of §0)

**Claim:** a single-CPU run of the ②.2 kernel behaves like today's uniprocessor kernel. **Harness:** run the existing uniprocessor task tests + a strict-priority + round-robin scenario under the ②.2 build at N=1; assert the task execution ORDER (a logged sequence of tskids) is identical to today's pre-②.2 kernel, and the boot reaches `Starting T-Kernel` + `Initial task started` (`main.c:361`+). **Falsifier:** a deliberately-broken per-CPU accessor (e.g. `CUR_CTXTSK` that reads the wrong slot) makes the N=1 ordering diverge → `SMP-UNIPROC: FAIL`. This is the guard that ②.2 didn't silently change the scheduler the uniprocessor sees.

---

## 5. The certs summary + falsifiers

| Cert | Claim | Falsifier (MUST go RED) | QEMU `-smp 4` | RPi3 `[live]` |
|---|---|---|---|---|
| `[smp-uniproc-semantics]` | N=1 ②.2 == today's uniproc (task order, RR, dispatch-disable) | broken per-CPU accessor → N=1 order diverges | **YES** | yes |
| `[smp-prod-N-tasks-run]` | N CPUs run DISTINCT **real T-Kernel TCBs** (not `smp_task` stand-ins) concurrently | don't per-CPU-ize `schedtsk` → only cpu0 advances | **YES** | yes |
| `[smp-async-preempt]` (②.2b) | a task is preempted **mid-compute** (not at a checkpoint) by the SGI → context saved+restored correctly, task resumes exactly | `-DSMP_NO_ASYNC` (handler only sets flag, no IRQ-return dispatch) → no mid-compute preempt; or a clobbered IAR slot → crash | **YES** | barrier/timing teeth |
| **`[smp-one-mind]` (CROWN)** | `r_forward` byte-identical uniproc vs SMP (N=2+ scheduling) | `-DSMP_ONEMIND_RACE` (unserialized `rc` scribble) → hash differs; `-DSMP_NO_RQLOCK` (§1.4 livelock) → dead mind | **YES** for determinism | **ONLY** for barrier coherency |
| `[smp-no-deadlock]` | ②.2 boots N CPUs into the **production** dispatcher, mixed real-task workload, no dead/livelock; BKL re-entrancy (timer IRQ nested in syscall) doesn't self-deadlock | `-DSMP_NO_RQLOCK` → `bitsearch1` livelock (§1.4) → watchdog FAIL | **YES** (incl. §1.4 falsifier) | yes |

All are QEMU `-smp 4`-testable (extend `tests/aarch64/run_smp{0,1}.sh` → `run_smp2.sh`, the boot-self-test + grep harness pattern). The **barrier/coherency teeth are `[live]`-only on RPi3** — the same MC-2 / ②.0 / ②.1a honesty (`smp.c:64-68`).

---

## 6. Sequencing — small, falsifiable, each shippable (awaiting mk_pino's go-ahead PER WAVE)

Each wave: a falsifiable cert + a falsifier that MUST go RED, on the **explicit-hash base `fd4900a9`**, via a **separate impl→audit cycle** (implementer ≠ auditor ≠ commander). **②.2's implementation awaits mk_pino's go-ahead per wave** — this plan makes it READY, it does not start it.

**②.2a — per-CPU `knl_ctxtsk`/`knl_schedtsk` + the PRODUCTION dispatcher schedules real tasks on 2 CPUs under the BKL, N=1 semantics preserved. (THE SMALLEST REAL ②.2 SLICE.)**
- Add `CUR_CTXTSK`/`CUR_SCHEDTSK` accessors (§2.2) — per-CPU under SMP, plain-global otherwise (x86/linux/rl78 + N=1 byte-identical).
- Add `schedtsk`/`dispatch_disabled`/`taskmode`/`taskindp` to `struct smp_cpu` (`smp.c:294`); init the per-CPU array in `knl_task_initialize` (`task.c:102`).
- Convert the **4 dangerous sites** (§2.2): the production dispatcher asm (`cpu_support.S:48-124` → per-CPU loads, §2.3), `END_CRITICAL_SECTION` (`cpu_status.h:19`), the timer handler (`timer.c:187-226`, §2.4), `in_indp`/`in_ddsp`/`in_qtsk` (`cpu_status.h:49-52`). Route `BEGIN/END_CRITICAL_SECTION` + `BEGIN/END_DISABLE_INTERRUPT` (`cpu_status.h:17,30`) through `bkl_acquire`/`bkl_release` (`smp.c:367-391`); **hand-convert the 4 bare-`DI` allocator sites** (`memory.c:275,303,364,384`).
- Graduate the bringup: `_secondary_dispatch_entry` (`start.S:284`) currently branches a secondary into `smp_dispatch_loop` (the sandbox C orchestrator over `smp_task`); ②.2a branches it into the **production `.Ldispatch_loop`** (`cpu_support.S:90`) with its per-CPU `g_smpcpu[me]` state — so the secondary runs **real T-Kernel TCBs** pulled from the one shared `knl_ready_queue` under the BKL, not `smp_task` stand-ins.
- Stage the remaining 140 `kernel/common/` "current task" reads → `CUR_CTXTSK` (§2.5), guarded by the certs.
- **No async preempt yet** — cross-CPU preemption is tick-bounded + cooperative (a CPU re-evaluates `CUR_SCHEDTSK` on its own timer tick or at `END_CRITICAL_SECTION`). The ②.1a SGI still only sets the flag.
- **Certs:** `[smp-uniproc-semantics]` (N=1 == today) + `[smp-prod-N-tasks-run]` (2 CPUs run distinct REAL TCBs) + `[smp-no-deadlock]`. *Proves the real scheduler runs real tasks on 2 cores under one lock, and N=1 still behaves like the uniprocessor kernel.*

**②.2b — true async register-context preempt via the SGI.**
- Make the IRQ-return path dispatch on `g_resched_pending[me]` (§3.2 Option A): reuse `knl_dispatch` (`cpu_support.S:61`) to save the interrupted task + switch to `CUR_SCHEDTSK`, gated by the same condition as `END_CRITICAL_SECTION`. Make `knl_reschedule` (`task.h:240-249`) IPI-aware (`smp_send_reschedule` the target CPU, §2.2).
- **Cert:** `[smp-async-preempt]` — a task is preempted **mid-compute** (a busy loop with NO checkpoint poll) by the SGI; prove the context is saved/restored and the task resumes exactly. **Falsifier `-DSMP_NO_ASYNC`:** the handler only sets the flag (the ②.1a behavior) → no mid-compute preempt → FAIL. This is the wave where the aarch64 IRQ-path C-ABI trap is the dominant risk; the auditor independently re-derives the EOIR-ordering + the IAR-slot non-clobber.

**②.2c — the `[smp-one-mind]` byte-identity CROWN cert.**
- Run the real bare-metal `r_forward` (fixed seed) under the ②.2 SMP kernel with N=2+ CPUs actually scheduling; prove the FNV-1a logit hash is byte-identical to N=1 (§4). One forward at a time (§1.2). Ship the `-DSMP_ONEMIND_RACE` falsifier (a shared-`rc` scribble must make the hash differ) + the `-DSMP_NO_RQLOCK` falsifier.
- This is the gate that says **"②.2 did not split the mind."** Until it's green, ②.2 is not trustworthy. Honest QEMU-masks-coherency caveat stated; barrier sub-claim `[live]`-deferred to RPi3.

**DEFERRED past ②.2 (explicit):**
- **②.3 — finer locks** (split BKL → `g_rqlock` → `g_memlock` → object-table locks, `full-smp-plan.md` §2.2) + **per-CPU run-queues + migration/load-balancing** (the global single ready-queue is correct + simple first; per-CPU queues *change* scheduling semantics). Re-run `[smp-one-mind]` after **every** split.
- **Concurrent MIND operations** — two `r_forward`s at once, or forward-while-training: needs per-call `rc` scratch or a **mind-lock** (the §1.2 race). NOT in ②.2. This is where the hosted-port teacher actually parallelizes (§7).
- **The `klocktsk` "one kernel-locked ready task" invariant** (`ready_queue.h:38`) — auto-correct under the BKL; needs explicit attention only at the finer-lock stage (`full-smp-plan.md` §3.4).
- **Hosted-port SMP** (threads-as-CPUs) — §7. **x86_64/rl78 bare-metal SMP** — ②.2 is aarch64-only. **CPU hotplug / NUMA** — N fixed at boot.

---

## 7. The hosted-port question (answered honestly) — where the mind ACTUALLY gets multicore

**The heavy mind (the teacher LLM) runs HOSTED, not bare-metal.** The bare-metal kernel dual-compiles only the SMALL R3/MoE/dtr math (`r_forward`, `moe.c`, `dtr.c`); the big teacher is hosted-only (per MEMORY.md teacher-student architecture). So:

**What bare-metal ②.2 proves:** the SMP *mechanism* — per-CPU scheduling, BKL serialization, async preempt, and byte-identity of the **small** R3 `r_forward` under real concurrency. It converts the **bare-metal** scheduler and certifies `[smp-one-mind]` on the small R3 mind. **It does NOT give the teacher LLM multicore speedup** — the teacher isn't bare-metal, and (per §1.2) ②.2 runs one mind forward at a time, so there is no parallel-mind throughput win on bare metal. **Bare-metal ②.2's value is the MECHANISM + the determinism proof, not throughput.**

**Where the mind's REAL multicore benefit lives: the hosted-port SMP.** The hosted port (threads-as-CPUs, `arch/linux/{aarch64,x86_64}/`) is where the teacher LLM runs and where parallelizing it across cores is the actual speedup. That is a **separate lift**:
- The hosted BKL is a recursive `pthread_mutex` (not the aarch64 ticket spinlock) — `arch_irq_disabled_flag` is explicitly "cannot protect across threads" (`arch/linux/aarch64/preempt.c`, per `full-smp-plan.md` §2.1).
- The IPI is a per-thread real-time signal (`pthread_kill(tid, SIGRESCHED)`), mirroring the existing `SIGEV_THREAD_ID` per-thread targeting (`smp-1-ipi-preempt-plan.md:275`).
- The hosted port currently *relies on* single-threaded kernel entry (the `SIGEV_THREAD_ID` retreat-fix, §1.4) — bringing it to true N-thread SMP undoes that retreat the disciplined way.

**Does the bare-metal ②.2 design carry over to it?** **The SHAPE carries; the primitives don't.** The per-CPU `CUR_CTXTSK`/`CUR_SCHEDTSK` accessor pattern (§2.2), the per-CPU-vs-global-ready-queue split (§2.2 B), the dispatch-on-IRQ-return preempt model (§3.2 A → on hosted: dispatch-on-signal-return), and the `[smp-one-mind]`/`[smp-uniproc-semantics]` cert structure all port directly. What changes is the lock primitive (spinlock→pthread_mutex), the IPI primitive (SGI→signal), and the "CPU" (core→thread). **Critically, the hosted port is where the §1.2 mind-lock for CONCURRENT mind operations must actually be built** — because that's where two threads genuinely want to run two forwards of the (big) mind at once. So the honest sequencing: **bare-metal ②.2 proves the mechanism + single-mind determinism; hosted-port SMP (a separate, later lift) is where the teacher LLM parallelizes and where concurrent-mind safety (the mind-lock / per-call scratch) gets built and certified.**

---

## 8. Honest cost + what stays DEFERRED (brutal honesty)

1. **②.2 is where the kernel genuinely becomes SMP — the riskiest, most invasive wave in the repo.** The shipped kernel is **no longer byte-identical** (§0); the guarantee shifts to **mind-output-identity + uniproc-semantics-preservation**. This is a real loss of the simplest possible safety story (ungated byte-identity) in exchange for the END GOAL. It is the "10-year problem" core. The disciplined claim: p-kernel can do it in **small, falsifiable waves** because (a) the mind's heavy-math determinism is already solved (§1.3, carried from `full-smp-plan.md`), reducing ②.2 to a kernel-state-race problem the BKL closes wholesale, and (b) ②.0/②.1a already built + proved the BKL, the per-CPU block, the bringup, and the SGI IPI in a sandbox — ②.2 *graduates* proven pieces, it does not invent them.

2. **The barrier/race teeth are only fully `[live]` on RPi3.** QEMU TCG models memory strongly and MAY MASK a missing `dsb ish`/SMPEN=0 (`smp.c:64-68`). A QEMU `[smp-one-mind]` green proves determinism-of-reordering + no-corruption; it does NOT prove the BKL/SGI barrier discipline on weakly-ordered silicon. **A QEMU green is NOT a hardware green.** RPi3 also isn't GICv2 (BCM2837 mailbox IPI) — the RPi3 `[live]` port needs the BCM2837 send path (`smp-1-ipi-preempt-plan.md:233`). MC-2.2 hardware validation is effectively a prerequisite for TRUSTING ②.2 on real hardware.

3. **What ②.2 must NOT silently lose** (the uniprocessor's current guarantees): **global strict priority** (one global ready-queue preserves it; per-CPU queues in ②.3 would change it — a semantic decision, not a silent regression); **`tk_dis_dsp()` semantics** (per-CPU `knl_dispatch_disabled` now means "not preempted *on its CPU*" — a documented narrowing, §2.2); **critical-section atomicity** (every `BEGIN_CRITICAL_SECTION` + the 4 bare-`DI` allocator sites under the BKL — enumerate ALL, §1.1); **byte-identity of the mind** (the crown, the standing `[smp-one-mind]` guard, re-run after every change).

4. **②.2's implementation awaits mk_pino's go-ahead, PER WAVE.** Read-only on code in this plan. The smallest real first slice is **②.2a** (per-CPU `CUR_CTXTSK`/`CUR_SCHEDTSK` + the production dispatcher schedules real T-Kernel tasks on 2 CPUs under the BKL + `[smp-uniproc-semantics]` proving N=1 is unchanged). Each wave is a **separate impl→audit cycle** (implementer ≠ auditor ≠ commander; the development METHOD is the project's immune system).

---

## Appendix — grounding (file:line, all on trunk `fd4900a9`)

**The honesty shift (§0):**
- Gating that gave ②.0/②.1a byte-identity: `smp.c:73` (`#ifdef SMP_SELFTEST`), `cpu_support.S:382`, `start.S:282`; the byte-identity claim `boot/aarch64/Makefile:459`; the production dispatcher untouched `cpu_support.S:90-124`; globals untouched `task.c:62-63`. The sandbox stand-ins (not real TCBs): `struct smp_task` `smp.c:409-414`.

**The production scheduler ②.2 converts (§2):**
- Scheduler globals: `task.c:62` (`knl_ctxtsk`, doc "Only task dispatcher changes ctxtsk" `task.h:166-173`), `:63` (`knl_schedtsk`, `task.h:175-181`), `:55` (`knl_dispatch_disabled`), `:64`/`:102` (`knl_ready_queue` + init). Site counts: 232 `knl_ctxtsk` / 72 `knl_schedtsk` / 283 combined; 140 reads in `kernel/common/`; only 1 production C write (`task.c:102`) — the rest are the asm dispatcher.
- `knl_reschedule` (sets `knl_schedtsk`, no-op `knl_dispatch_request`) `task.h:240-249`; `knl_dispatch_request()` = no-op `cpu_status.h:59`; `knl_make_ready`→sets `schedtsk` `task.c:230`; `knl_make_non_ready` `task.c:256-257`.
- Per-CPU candidates `knl_taskmode`/`knl_taskindp`: defs `sysinfo_depend.h:32-33`; dispatcher save/restore `cpu_support.S:73-75,114-115`; timer asm bump `cpu_support.S:157-167`.
- Ready queue (bitmap + `top_priority` + `klocktsk` + the uniproc invariant comment): `ready_queue.h:38,44-50,73-81,98-114`.

**The dispatcher + critical sections (§2.3, §3):**
- The production dispatcher (per-CPU-ize): `cpu_support.S:48-124` (discard-switch `:48-53`, save `:61-85`, loop `:90-124`, idle `wfe` `:120-124`).
- `BEGIN/END_CRITICAL_SECTION` (the dispatch test `knl_ctxtsk != knl_schedtsk`): `cpu_status.h:17-24`; `BEGIN/END_DISABLE_INTERRUPT` `:30-31`; `in_indp/in_ddsp/in_qtsk` `:49-52`; `CTXB` `:69-71`.
- `disint`/`enaint` (mrs/msr daif): `arch/aarch64/include/cpu_insn.h` (per `full-smp-plan.md` §1.2).
- The 4 bare-`DI` allocator sites: `memory.c:275,303,364,384`.
- Timer handler (reads `knl_ctxtsk`, RR-rotates): `timer.c:177-231` (`BEGIN` `:183`, time charge `:187-194`, RR `:219-226`, `END` `:228`); `knl_timer_handler_startup` asm `cpu_support.S:151-170`.
- The IRQ vector (where async preempt threads in): `_vec_el1_irq` `cpu_support.S:234-284` (save `:235`, IAR slot `:240,273-274`, `knl_intvec` dispatch `:264-269`, EOIR `:280`, `eret` `:284`).

**The ②.0/②.1a substrate ②.2 graduates (the sandbox):**
- BKL: `smp.c:341-391` (`g_bkl_lock`/`g_bkl_owner`/`g_bkl_depth`, recursive `bkl_acquire`/`bkl_release`); the no-self-deadlock owner check `:372-375`.
- Per-CPU block `g_smpcpu[]` (ctxtsk/schedtsk off 0/8, +exec/cpu_id/live/preempted_at/highprio_ran): `smp.c:294-320`; `smp_this_cpu` (MPIDR Aff0) `:323`; asm per-CPU load `smp_cur_tcb_load` `cpu_support.S:402-410`; offsets `SMPCPU_*` `cpu_support.S:390-392` + `smp.c:308-316`.
- The one shared ready list (graduate to `knl_ready_queue`): `g_ready[]`/`smp_ready_push`/`smp_ready_pull` `smp.c:417-447`.
- The ②.1a SGI IPI (make it switch context in ②.2b): `smp_send_reschedule` `smp.c:205-220` (+`-DSMP_NO_IPI` no-op `:207-211`); `smp_gic_cpuif_init` `:226-233`; `smp_resched_sgi_handler` (only sets the flag — ②.2 makes it switch) `:242-250`; `g_resched_pending[]` `:198`; the cooperative-at-checkpoint preempt loop (the thing ②.2b replaces) `:645-702`; the explicit "true async preempt DEFERRED to ②.2" `smp.c:617-620`.
- Bringup: `smp_bringup_secondary` (PSCI CPU_ON → `_secondary_dispatch_entry`) `smp.c:495-508`; `_secondary_dispatch_entry`/`_secondary_disp_el1_setup` (→ `smp_dispatch_loop`, ②.2 → `.Ldispatch_loop`) `start.S:282-350`; `smp_dispatch_loop` `cpu_support.S:412-421`; `smp_dispatch_run` orchestrator `smp.c:707-740`.

**The crown cert (§4):**
- The bare-metal mind forward (pure math, module-static buffers): `r_forward` `r3_incontext.c:192-258`; weights `static float rw[R_NP]` `:141`; scratch `static R_TC rc` `:161` (THE §1.2 shared-scratch race surface); logits `rc.probs` `:253-254`; `r_forward` callers `:407,456,496,994,1443,3508`.
- The FNV-1a logit hash idiom (reuse for the cert): `ss6live_logit_hash` `student_shell.c:689-696`.
- The determinism-critical mind consumers (already insulated, §1.3): DMN ring/replay `lm_consolidate.c`; `gl_merge` SUM + ~1e-6 IEEE asterisk `gossip_learn.c`; `-ffp-contract=off` build flag `boot/aarch64/Makefile:60,65`.

**The cert harness pattern (§4, §5):**
- ②.0/②.1a test scripts (model `run_smp2.sh` on these): `tests/aarch64/run_smp0.sh` (build `-DSMP_SELFTEST`, grep `SMP-RUN/MUTEX/BOOT PASS`, `-DSMP_MUTEX_NOLOCK` falsifier), `tests/aarch64/run_smp1.sh` (`-DSMP_PREEMPT_TEST`, grep `SMP-PREEMPT: PASS`, `-DSMP_NO_IPI` falsifier `:70-88`).
- Makefile SMP targets: `run-smp0`/`run-smp0-nolock` `boot/aarch64/Makefile:460-470`; `QEMU_SMP_FLAGS` (`-smp 4`) `:368`; the byte-identity claim `:459`; `-ffp-contract=off` `:60-65`.
- The boot self-test driver (runs BEFORE `knl_t_kernel_main`): `boot/aarch64/main.c:312-368` (②.0 driver), `:361` (`Starting T-Kernel`).

**The hosted-port carry-over (§7):**
- `arch_irq_disabled_flag` "cannot protect across threads" + the `SIGEV_THREAD_ID` retreat-fix: `arch/linux/aarch64/preempt.c` (per `full-smp-plan.md` §2.1, §1.4); hosted `knl_ctxtsk`/`knl_schedtsk` sites (NOT touched by aarch64 ②.2): `arch/linux/{aarch64,x86_64}/{preempt.c,poc_dispatch.c,cpu_support.S}`.

---

**Note:** This plan is a DESIGN PLAN on trunk `fd4900a9` (②.0 + ②.1a merged + audited). ②.2 is the most invasive change in the repo and a 本丸-level decision **awaiting mk_pino's go-ahead, PER WAVE**, via **separate impl→audit cycles** (implementer ≠ auditor ≠ commander; the development METHOD is the project's immune system). This plan makes ②.2 READY and de-risked — it does **not** start it. The smallest real first slice is **②.2a** (per-CPU `CUR_CTXTSK`/`CUR_SCHEDTSK` + the production dispatcher schedules real T-Kernel tasks on 2 CPUs under the BKL; `[smp-uniproc-semantics]` proves N=1 is byte-behavior-identical to today; `[smp-prod-N-tasks-run]` proves 2 cores run distinct real TCBs). The crown `[smp-one-mind]` (the real `r_forward` byte-identical uniproc vs SMP) is **②.2c** — the gate that says ②.2 did not split the mind.
